open Lantern

let checks = ref 0
let failures = ref 0

let check name condition =
  incr checks;
  if condition then Printf.printf "  [ ok ] %s\n%!" name
  else begin
    incr failures;
    Printf.printf "  [FAIL] %s\n%!" name
  end

let section name = Printf.printf "\n%s\n%!" name

let expect_ok name = function
  | Ok value ->
    check name true;
    Some value
  | Error error ->
    check (name ^ " :: " ^ Nvme.string_of_error error) false;
    None

let expect_error name = function
  | Ok _ ->
    check (name ^ " :: Unexpectedly succeeded") false;
    None
  | Error error ->
    check name true;
    Some error

let image = "test.img"

let config ?(interrupts = false) ?(block_size = 512) () =
  {
    Nvme.default_config with
    Nvme.transport = Nvme.Mock;
    image_path = image;
    capacity_bytes = Int64.of_int (64 * 1024 * 1024);
    block_size;
    namespace_count = 4;
    use_interrupts = interrupts;
  }

let with_device ?(interrupts = false) ?(block_size = 512) name body =
  match Nvme.open_device ~config:(config ~interrupts ~block_size ()) Defaults.bdf with
  | Error error ->
    check (name ^ " :: " ^ Nvme.string_of_error error) false
  | Ok device ->
    (try body device with exn -> Nvme.close device; raise exn);
    Nvme.close device

let pure_status_tests () =
  section "Status decoding";
  let success = Status.decode 0 in
  check "Zero status decodes to success" (Status.is_success success.Status.status);
  let out_of_range = Status.decode ((1 lsl 14) lor 0x80) in
  check "LBA out of range decodes"
    (out_of_range.Status.status = Status.Generic Status.Lba_out_of_range);
  check "Do not retry bit decodes" out_of_range.Status.do_not_retry;
  let queue_identifier = Status.decode ((1 lsl 8) lor 0x01) in
  check "Command specific status decodes"
    (queue_identifier.Status.status
     = Status.Command_specific Status.Invalid_queue_identifier);
  let media = Status.decode ((2 lsl 8) lor 0x81) in
  check "Media status decodes"
    (media.Status.status = Status.Media Status.Unrecovered_read_error);
  let vendor = Status.decode ((7 lsl 8) lor 0x42) in
  check "Vendor specific status decodes"
    (vendor.Status.status = Status.Vendor_specific 0x42);
  let unknown = Status.decode ((0 lsl 8) lor 0x77) in
  check "Unknown generic status is preserved"
    (unknown.Status.status = Status.Generic (Status.Unknown_generic 0x77));
  check "Descriptions are non empty" (String.length (Status.describe out_of_range) > 0)

let pure_command_tests () =
  section "Command encoding";
  let read = Cmd.read ~cid:5 ~nsid:1 ~lba:0x1234L ~blocks:8 in
  check "Read opcode" (Int32.to_int (Cmd.get_dword read 0) land 0xff = Cmd.io_read);
  check "Read command id"
    ((Int32.to_int (Cmd.get_dword read 0) lsr 16) land 0xffff = 5);
  check "Read namespace" (Int32.to_int (Cmd.get_dword read 1) = 1);
  check "Read start block" (Int32.to_int (Cmd.get_dword read 10) = 0x1234);
  check "Read block count is zero based" (Int32.to_int (Cmd.get_dword read 12) = 7);
  let create_cq =
    Cmd.create_io_completion_queue ~cid:1 ~qid:3 ~depth:64 ~iova:0x1000L ~vector:3
      ~interrupts:true
  in
  check "Create CQ identifier and size"
    (Int32.to_int (Cmd.get_dword create_cq 10) = ((63 lsl 16) lor 3));
  check "Create CQ interrupt vector and flags"
    (Int32.to_int (Cmd.get_dword create_cq 11) = ((3 lsl 16) lor 0x3));
  let create_sq =
    Cmd.create_io_submission_queue ~cid:2 ~qid:3 ~depth:64 ~cqid:3 ~iova:0x2000L
  in
  check "Create SQ links to completion queue"
    (Int32.to_int (Cmd.get_dword create_sq 11) = ((3 lsl 16) lor 0x1));
  let queues = Cmd.set_number_of_queues ~cid:0 ~submission:4 ~completion:4 in
  check "Set features number of queues"
    (Int32.to_int (Cmd.get_dword queues 11) = ((3 lsl 16) lor 3))

let identify_tests () =
  section "Identify and health";
  with_device "Identify device" (fun device ->
      check "Backend reports the mock transport" (Nvme.backend_name device = "mock");
      let registers = Nvme.registers device in
      check "Controller is ready"
        (Int32.logand registers.Ffi.csts 1l = 1l);
      check "Controller advertises queue entries" (registers.Ffi.max_queue_entries >= 2);
      (match expect_ok "Identify controller" (Nvme.identify_controller device) with
       | None -> ()
       | Some controller ->
         check "Serial matches" (controller.Id.serial = "DEADBEEF");
         check "Model matches" (controller.Id.model = "lantern mock nvme");
         check "Namespace count" (controller.Id.namespace_count = 4);
         check "Max transfer shift" (controller.Id.max_transfer_shift = 9));
      (match expect_ok "Active namespace list" (Nvme.active_namespaces device) with
       | None -> ()
       | Some namespaces -> check "Namespace one is active" (namespaces = [ 1 ]));
      (match expect_ok "Identify namespace" (Nvme.identify_namespace device ~nsid:1) with
       | None -> ()
       | Some namespace ->
         check "Block size" (Id.block_size namespace = 512);
         check "Namespace holds half the capacity"
           (Int64.mul namespace.Id.size_blocks 512L = Int64.of_int (32 * 1024 * 1024)));
      (match expect_ok "SMART log" (Nvme.smart_log device) with
       | None -> ()
       | Some smart ->
         check "Temperature is plausible"
           (smart.Id.composite_temperature_kelvin > 250
            && smart.Id.composite_temperature_kelvin < 400));
      match expect_ok "Set number of queues" (Nvme.set_number_of_queues device ~submission:4 ~completion:4) with
      | None -> ()
      | Some (sq, cq) -> check "Queue grant is sane" (sq >= 1 && cq >= 1))

let block_size_of device nsid =
  match Nvme.identify_namespace device ~nsid with
  | Ok namespace -> Id.block_size namespace
  | Error _ -> 512

let io_tests () =
  section "Block transfers";
  with_device "Block transfer device" (fun device ->
      match expect_ok "Create I/O queue" (Nvme.create_io_queue ~depth:16 device) with
      | None -> ()
      | Some queue ->
        let block = block_size_of device 1 in
        let payload = Bytes.init (block * 8) (fun i -> Char.chr ((i * 31) land 0xff)) in
        ignore
          (expect_ok "Write eight blocks"
             (Nvme.write_blocks device queue ~nsid:1 ~lba:64L ~data:payload));
        (match
           expect_ok "Read eight blocks" (Nvme.read_blocks device queue ~nsid:1 ~lba:64L ~blocks:8)
         with
         | None -> ()
         | Some readback -> check "Round trip matches" (Bytes.equal payload readback));
        let large = Bytes.init (128 * 1024) (fun i -> Char.chr ((i * 7) land 0xff)) in
        ignore
          (expect_ok "Write 128 KiB across a PRP list"
             (Nvme.write_blocks device queue ~nsid:1 ~lba:1024L ~data:large));
        (match
           expect_ok "Read 128 KiB across a PRP list"
             (Nvme.read_blocks device queue ~nsid:1 ~lba:1024L ~blocks:(128 * 1024 / block))
         with
         | None -> ()
         | Some readback -> check "Large round trip matches" (Bytes.equal large readback));
        ignore
          (expect_ok "Write zeroes"
             (Nvme.write_zeroes device queue ~nsid:1 ~lba:64L ~blocks:8));
        (match
           expect_ok "Read zeroed range" (Nvme.read_blocks device queue ~nsid:1 ~lba:64L ~blocks:8)
         with
         | None -> ()
         | Some readback ->
           check "Zeroed range reads back as zeros"
             (Bytes.equal readback (Bytes.make (block * 8) '\000')));
        ignore (expect_ok "Flush" (Nvme.flush device queue ~nsid:1));
        (match
           expect_error "Read past the end of the namespace"
             (Nvme.read_blocks device queue ~nsid:1 ~lba:100_000_000L ~blocks:1)
         with
         | Some (Nvme.Controller { status; _ }) ->
           check "Controller reports LBA out of range"
             (status.Status.status = Status.Generic Status.Lba_out_of_range)
         | _ -> check "Controller reports LBA out of range" false);
        (match
           expect_error "Read from a namespace that does not exist"
             (Nvme.read_blocks device queue ~nsid:3 ~lba:0L ~blocks:1)
         with
         | Some _ -> ()
         | None -> ());
        (match
           expect_error "Transfer larger than the queue slot"
             (Nvme.read_blocks device queue ~nsid:1 ~lba:0L ~blocks:(1024 * 1024))
         with
         | Some (Nvme.Bad_argument _) -> check "Oversized transfer is rejected locally" true
         | _ -> check "Oversized transfer is rejected locally" false);
        ignore (expect_ok "Delete I/O queue" (Nvme.delete_io_queue device queue)))

let tag_tests () =
  section "Tag and outstanding request tracking";
  with_device "Tag device" (fun device ->
      match expect_ok "Create shallow queue" (Nvme.create_io_queue ~depth:8 device) with
      | None -> ()
      | Some queue ->
        let block = block_size_of device 1 in
        let rec fill index acc =
          if index >= 32 then (index, acc)
          else
            match
              Nvme.Pipeline.submit_read device queue ~nsid:1 ~lba:0L ~blocks:1 ~block_bytes:block
            with
            | Ok slot -> fill (index + 1) (slot :: acc)
            | Error (Nvme.Driver { code = Ffi.Queue_full; _ }) -> (index, acc)
            | Error error ->
              check ("Unexpected submission failure :: " ^ Nvme.string_of_error error) false;
              (index, acc)
        in
        let submitted, _ = fill 0 [] in
        check "Queue accepts depth minus one commands" (submitted = Nvme.queue_depth queue - 1);
        check "Outstanding count matches submissions"
          (Nvme.queue_outstanding queue = submitted);
        let rec drain reaped =
          if reaped >= submitted then reaped
          else
            match Nvme.Pipeline.reap device queue 1000 with
            | Ok (Some _) -> drain (reaped + 1)
            | Ok None -> reaped
            | Error error ->
              check ("Unexpected completion failure :: " ^ Nvme.string_of_error error) false;
              reaped
        in
        let reaped = drain 0 in
        check "Every outstanding command completes" (reaped = submitted);
        check "Tags are returned to the pool" (Nvme.queue_outstanding queue = 0);
        ignore
          (expect_ok "Queue accepts work again"
             (Nvme.read_blocks device queue ~nsid:1 ~lba:0L ~blocks:1));
        ignore (expect_ok "Delete shallow queue" (Nvme.delete_io_queue device queue)))

let namespace_management_tests () =
  section "Namespace management";
  with_device "Namespace management device" (fun device ->
      let created = Nvme.create_namespace device ~blocks:2048L ~lba_format:0 in
      match expect_ok "Create namespace" created with
      | None -> ()
      | Some nsid ->
        check "New namespace identifier is allocated" (nsid > 1);
        (match
           expect_error "Unattached namespace is invisible" (Nvme.identify_namespace device ~nsid)
         with
         | Some _ -> ()
         | None -> ());
        ignore (expect_ok "Attach namespace" (Nvme.attach_namespace device ~nsid));
        (match expect_ok "Identify attached namespace" (Nvme.identify_namespace device ~nsid) with
         | None -> ()
         | Some namespace -> check "New namespace size" (namespace.Id.size_blocks = 2048L));
        (match expect_ok "Namespace list grows" (Nvme.active_namespaces device) with
         | None -> ()
         | Some list -> check "New namespace appears in the list" (List.mem nsid list));
        (match expect_ok "Create I/O queue for new namespace" (Nvme.create_io_queue ~depth:8 device) with
         | None -> ()
         | Some queue ->
           let payload = Bytes.make 512 'n' in
           ignore
             (expect_ok "Write to the new namespace"
                (Nvme.write_blocks device queue ~nsid ~lba:0L ~data:payload));
           (match
              expect_ok "Read from the new namespace"
                (Nvme.read_blocks device queue ~nsid ~lba:0L ~blocks:1)
            with
            | None -> ()
            | Some readback -> check "New namespace round trip" (Bytes.equal payload readback));
           ignore (expect_ok "Delete the queue" (Nvme.delete_io_queue device queue)));
        ignore (expect_ok "Detach namespace" (Nvme.detach_namespace device ~nsid));
        ignore (expect_ok "Delete namespace" (Nvme.delete_namespace device ~nsid));
        (match expect_ok "Namespace list shrinks" (Nvme.active_namespaces device) with
         | None -> ()
         | Some list -> check "Deleted namespace is gone" (not (List.mem nsid list))))

let multi_queue_tests () =
  section "Multiple I/O queues";
  with_device "Multi queue device" (fun device ->
      let first = Nvme.create_io_queue ~depth:16 device in
      let second = Nvme.create_io_queue ~depth:16 device in
      match (expect_ok "Create first queue" first, expect_ok "Create second queue" second) with
      | Some queue_a, Some queue_b ->
        check "Queue identifiers differ" (Nvme.queue_id queue_a <> Nvme.queue_id queue_b);
        let block = block_size_of device 1 in
        let slots =
          List.filter_map
            (fun (queue, lba) ->
              match
                Nvme.Pipeline.submit_write device queue ~nsid:1 ~lba ~blocks:1 ~block_bytes:block
              with
              | Ok slot -> Some (queue, slot)
              | Error _ -> None)
            [ (queue_a, 0L); (queue_b, 8L); (queue_a, 16L); (queue_b, 24L) ]
        in
        check "All queues accepted work" (List.length slots = 4);
        let reaped =
          List.fold_left
            (fun acc (queue, _) ->
              match Nvme.Pipeline.reap device queue 1000 with
              | Ok (Some _) -> acc + 1
              | _ -> acc)
            0 slots
        in
        check "Completions arrive on both queues" (reaped = 4);
        ignore (expect_ok "Delete first queue" (Nvme.delete_io_queue device queue_a));
        ignore (expect_ok "Delete second queue" (Nvme.delete_io_queue device queue_b))
      | _ -> ())

let interrupt_tests () =
  section "MSI-X completion signaling";
  with_device ~interrupts:true "Interrupt device" (fun device ->
      check "Interrupts are enabled" (Nvme.interrupts_enabled device);
      match expect_ok "Create interrupt driven queue" (Nvme.create_io_queue ~depth:16 device) with
      | None -> ()
      | Some queue ->
        check "Queue has an interrupt vector" (Nvme.queue_vector queue >= 0);
        let payload = Bytes.make 1024 'i' in
        ignore
          (expect_ok "Write through the interrupt path"
             (Nvme.write_blocks device queue ~nsid:1 ~lba:512L ~data:payload));
        (match
           expect_ok "Read through the interrupt path"
             (Nvme.read_blocks device queue ~nsid:1 ~lba:512L ~blocks:2)
         with
         | None -> ()
         | Some readback -> check "Interrupt driven round trip matches" (Bytes.equal payload readback));
        ignore (expect_ok "Delete interrupt driven queue" (Nvme.delete_io_queue device queue)))

let reset_tests () =
  section "Controller reset";
  with_device "Reset device" (fun device ->
      (match expect_ok "Create queue before reset" (Nvme.create_io_queue ~depth:8 device) with
       | None -> ()
       | Some queue ->
         let payload = Bytes.make 512 'r' in
         ignore
           (expect_ok "Write before reset"
              (Nvme.write_blocks device queue ~nsid:1 ~lba:2048L ~data:payload)));
      ignore (expect_ok "Reset controller" (Nvme.reset device));
      let registers = Nvme.registers device in
      check "Controller is ready after reset" (Int32.logand registers.Ffi.csts 1l = 1l);
      ignore (expect_ok "Identify after reset" (Nvme.identify_controller device));
      match expect_ok "Create queue after reset" (Nvme.create_io_queue ~depth:8 device) with
      | None -> ()
      | Some queue -> (
        match expect_ok "Data survives the reset" (Nvme.read_blocks device queue ~nsid:1 ~lba:2048L ~blocks:1) with
        | None -> ()
        | Some readback -> check "Data written before reset is intact" (Bytes.get readback 0 = 'r')))

let four_kilobyte_namespace_tests () =
  section "Four kilobyte logical blocks";
  with_device ~block_size:4096 "Large block device" (fun device ->
      match expect_ok "Identify namespace" (Nvme.identify_namespace device ~nsid:1) with
      | None -> ()
      | Some namespace -> (
        check "Block size is four kilobytes" (Id.block_size namespace = 4096);
        match expect_ok "Create queue" (Nvme.create_io_queue ~depth:8 device) with
        | None -> ()
        | Some queue ->
          let payload = Bytes.init 8192 (fun i -> Char.chr ((i * 13) land 0xff)) in
          ignore
            (expect_ok "Write two large blocks"
               (Nvme.write_blocks device queue ~nsid:1 ~lba:4L ~data:payload));
          (match
             expect_ok "Read two large blocks"
               (Nvme.read_blocks device queue ~nsid:1 ~lba:4L ~blocks:2)
           with
           | None -> ()
           | Some readback -> check "Large block round trip matches" (Bytes.equal payload readback));
          ignore (expect_ok "Delete queue" (Nvme.delete_io_queue device queue))))

let () =
  Printf.printf "lantern OCaml test suite\n%!";
  pure_status_tests ();
  pure_command_tests ();
  identify_tests ();
  io_tests ();
  tag_tests ();
  namespace_management_tests ();
  multi_queue_tests ();
  interrupt_tests ();
  reset_tests ();
  four_kilobyte_namespace_tests ();
  Printf.printf "\n%d checks, %d failures\n%!" !checks !failures;
  exit (if !failures = 0 then 0 else 1)
