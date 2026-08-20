type error =
  | Driver of { code : Ffi.error_code; message : string }
  | Controller of { status : Status.decoded; command_id : int }
  | Timeout of string
  | Bad_argument of string

let string_of_error = function
  | Driver { code; message } ->
    Printf.sprintf "Driver error %s: %s" (Ffi.error_code_name code) message
  | Controller { status; command_id } ->
    Printf.sprintf "Command %d failed: %s" command_id (Status.describe status)
  | Timeout what -> Printf.sprintf "Timed out waiting for %s" what
  | Bad_argument what -> Printf.sprintf "Invalid request: %s" what

type transport = Vfio | Mock

type config = {
  transport : transport;
  admin_queue_depth : int;
  use_interrupts : bool;
  image_path : string;
  capacity_bytes : int64;
  block_size : int;
  namespace_count : int;
  latency_us : int;
  serial : string;
  model : string;
}

let default_config = {
  transport = Mock;
  admin_queue_depth = 64;
  use_interrupts = false;
  image_path = "mock.img";
  capacity_bytes = Int64.of_int (256 * 1024 * 1024);
  block_size = 512;
  namespace_count = 4;
  latency_us = 0;
  serial = "DEADBEEF";
  model = "lantern mock nvme";
}

type request = {
  command_id : int;
  submitted_at : float;
  mutable completed_at : float;
  mutable outcome : Status.decoded option;
  mutable result_dword : int32;
}

type queue = {
  qid : int;
  depth : int;
  vector : int;
  data_buffer : int;
  slot_bytes : int;
  free_tags : int Stack.t;
  outstanding : (int, request) Hashtbl.t;
  mutable interrupt : Unix.file_descr option;
  mutable live : bool;
}

type t = {
  handle : Ffi.device;
  bdf : string;
  transport : transport;
  scratch : int;
  admin : queue;
  mutable io_queues : queue list;
  mutable next_qid : int;
  mutable closed : bool;
  namespace_cache : (int, Id.namespace) Hashtbl.t;
  mutable interrupt_vectors : int;
}

let max_queues = 16
let scratch_bytes = 4096
let default_slot_bytes = 128 * 1024

let driver_error device code =
  let message = Ffi.dev_error device in
  Driver
    {
      code = Ffi.decode_error_code code;
      message = (if message = "" then Ffi.strerror code else message);
    }

let make_queue ~qid ~depth ~vector ~data_buffer ~slot_bytes =
  let free_tags = Stack.create () in
  for tag = depth - 1 downto 0 do
    Stack.push tag free_tags
  done;
  {
    qid;
    depth;
    vector;
    data_buffer;
    slot_bytes;
    free_tags;
    outstanding = Hashtbl.create depth;
    interrupt = None;
    live = true;
  }

let acquire_tag queue =
  if Stack.is_empty queue.free_tags then None else Some (Stack.pop queue.free_tags)

let release_tag queue tag = Stack.push tag queue.free_tags

let inflight queue = Hashtbl.length queue.outstanding

let ffi_result device code value =
  if code = Ffi.ok then Ok value else Error (driver_error device code)

let open_device ?(config = default_config) bdf =
  let ffi_config =
    {
      Ffi.backend =
        (match config.transport with
         | Vfio -> Ffi.backend_vfio
         | Mock -> Ffi.backend_mock);
      admin_queue_depth = config.admin_queue_depth;
      enable_msix = config.use_interrupts;
      mock_image_path = config.image_path;
      mock_capacity_bytes = config.capacity_bytes;
      mock_lba_bytes = config.block_size;
      mock_namespaces = config.namespace_count;
      mock_latency_us = config.latency_us;
      mock_serial = config.serial;
      mock_model = config.model;
    }
  in
  match Ffi.dev_open bdf ffi_config with
  | Error message -> Error (Driver { code = Ffi.Backend; message })
  | Ok handle -> (
    match Ffi.dma_alloc handle scratch_bytes with
    | Error message -> 
      ignore (Ffi.dev_close handle);
      Error (Driver { code = Ffi.No_memory; message })
    | Ok scratch ->
      let admin =
        make_queue ~qid:0 ~depth:config.admin_queue_depth ~vector:0 ~data_buffer:scratch
          ~slot_bytes:scratch_bytes
      in
      let device =
        {
          handle;
          bdf;
          transport = config.transport;
          scratch;
          admin;
          io_queues = [];
          next_qid = 1;
          closed = false;
          namespace_cache = Hashtbl.create 8;
          interrupt_vectors = (if config.use_interrupts then Ffi.irq_count handle else 0);
        }
      in
      if config.use_interrupts then
        admin.interrupt <- Ffi.irq_file_descr handle 0;
      Ok device)

let close device =
  if not device.closed then begin
    device.closed <- true;
    List.iter (fun queue -> queue.live <- false) device.io_queues;
    ignore (Ffi.dev_close device.handle)
  end

let backend_name device = Ffi.dev_backend device.handle
let bdf device = device.bdf
let registers device = Ffi.dev_state device.handle
let queue_registers device qid = Ffi.queue_state device.handle qid
let interrupts_enabled device = device.interrupt_vectors > 0

let drain_interrupt = function
  | None -> ()
  | Some fd ->
    let buffer = Bytes.create 8 in
    (try ignore (Unix.read fd buffer 0 8) with Unix.Unix_error _ -> ())

let wait_for_interrupt queue timeout_ms =
  match queue.interrupt with
  | None -> ()
  | Some fd -> (
    let timeout = if timeout_ms < 0 then -1.0 else float_of_int timeout_ms /. 1000.0 in
    match Unix.select [ fd ] [] [] timeout with
    | [ _ ], _, _ -> drain_interrupt queue.interrupt
    | _ -> ()
    | exception Unix.Unix_error (Unix.EINTR, _, _) -> ())

let poll_raw device queue timeout_ms =
  if queue.qid = 0 then Ffi.admin_poll device.handle timeout_ms
  else Ffi.io_poll device.handle queue.qid timeout_ms

let reap_once device queue timeout_ms =
  let outcome =
    match queue.interrupt with
    | None -> poll_raw device queue timeout_ms
    | Some _ -> (
      match poll_raw device queue 0 with
      | Ffi.Idle when timeout_ms <> 0 ->
        wait_for_interrupt queue timeout_ms;
        poll_raw device queue 0
      | other -> other)
  in
  match outcome with
  | Ffi.Idle -> Ok None
  | Ffi.Failed code -> Error (driver_error device.handle code)
  | Ffi.Completed cqe -> (
    match Hashtbl.find_opt queue.outstanding cqe.Ffi.cid with
    | None ->
      Error (Bad_argument (Printf.sprintf "Completion for unknown command identifier %d" cqe.Ffi.cid))
    | Some request ->
      Hashtbl.remove queue.outstanding cqe.Ffi.cid;
      release_tag queue cqe.Ffi.cid;
      request.outcome <- Some (Status.decode cqe.Ffi.status);
      request.result_dword <- cqe.Ffi.dw0;
      request.completed_at <- Unix.gettimeofday ();
      Ok (Some request))

let submit device queue sqe =
  match acquire_tag queue with
  | None -> Error (Driver { code = Ffi.Queue_full; message = "No free command identifiers" })
  | Some tag ->
    Cmd.set_opcode_cid sqe
      ~opcode:(Int32.to_int (Cmd.get_dword sqe 0) land 0xff)
      ~cid:tag;
    let code =
      if queue.qid = 0 then Ffi.admin_submit device.handle sqe
      else Ffi.io_submit device.handle queue.qid sqe
    in
    if code <> Ffi.ok then begin
      release_tag queue tag;
      Error (driver_error device.handle code)
    end
    else begin
      let request =
        {
          command_id = tag;
          submitted_at = Unix.gettimeofday ();
          completed_at = 0.0;
          outcome = None;
          result_dword = 0l;
        }
      in
      Hashtbl.replace queue.outstanding tag request;
      Ok request
    end

let rec wait_for device queue request deadline =
  match request.outcome with
  | Some outcome -> Ok outcome
  | None ->
    if Unix.gettimeofday () > deadline then
      Error (Timeout (Printf.sprintf "command %d on queue %d" request.command_id queue.qid))
    else (
      match reap_once device queue 1 with
      | Error error -> Error error
      | Ok _ -> wait_for device queue request deadline)

let execute ?(timeout_ms = 5000) device queue sqe =
  match submit device queue sqe with
  | Error error -> Error error
  | Ok request -> (
    let deadline = Unix.gettimeofday () +. (float_of_int timeout_ms /. 1000.0) in
    match wait_for device queue request deadline with
    | Error error -> Error error
    | Ok outcome ->
      if Status.is_success outcome.Status.status then Ok request
      else Error (Controller { status = outcome; command_id = request.command_id }))

let scratch_iova device = Ffi.dma_iova device.handle device.scratch

let read_scratch device length =
  let buffer = Bytes.create length in
  let code = Ffi.dma_read device.handle device.scratch 0 buffer 0 length in
  if code = Ffi.ok then Ok buffer else Error (driver_error device.handle code)

let write_scratch device source =
  let length = Bytes.length source in
  let code = Ffi.dma_write device.handle device.scratch 0 source 0 length in
  ffi_result device.handle code ()

let identify_controller device =
  let sqe = Cmd.identify_controller ~cid:0 ~iova:(scratch_iova device) in
  match execute device device.admin sqe with
  | Error error -> Error error
  | Ok _ -> (
    match read_scratch device 4096 with
    | Error error -> Error error
    | Ok buffer -> Ok (Id.parse_controller buffer))

let identify_namespace device ~nsid =
  let sqe = Cmd.identify_namespace ~cid:0 ~nsid ~iova:(scratch_iova device) in
  match execute device device.admin sqe with
  | Error error -> Error error
  | Ok _ -> (
    match read_scratch device 4096 with
    | Error error -> Error error
    | Ok buffer ->
      let namespace = Id.parse_namespace buffer in
      Hashtbl.replace device.namespace_cache nsid namespace;
      Ok namespace)

let active_namespaces device =
  let sqe = Cmd.identify_namespace_list ~cid:0 ~start:0 ~iova:(scratch_iova device) in
  match execute device device.admin sqe with
  | Error error -> Error error
  | Ok _ -> (
    match read_scratch device 4096 with
    | Error error -> Error error
    | Ok buffer ->
      let rec collect index acc =
        if index >= 1024 then List.rev acc
        else
          let nsid = Int32.to_int (Bytes.get_int32_le buffer (index * 4)) in
          if nsid = 0 then List.rev acc else collect (index + 1) (nsid :: acc)
      in
      Ok (collect 0 []))

let smart_log device =
  let sqe = Cmd.smart_log ~cid:0 ~iova:(scratch_iova device) ~dwords:128 in
  match execute device device.admin sqe with
  | Error error -> Error error
  | Ok _ -> (
    match read_scratch device 512 with
    | Error error -> Error error
    | Ok buffer -> Ok (Id.parse_smart buffer))

let set_number_of_queues device ~submission ~completion =
  let sqe = Cmd.set_number_of_queues ~cid:0 ~submission ~completion in
  match execute device device.admin sqe with
  | Error error -> Error error
  | Ok request ->
    let value = Int32.to_int request.result_dword in
    Ok ((value land 0xffff) + 1, ((value lsr 16) land 0xffff) + 1)

let block_size_of device ~nsid =
  match Hashtbl.find_opt device.namespace_cache nsid with
  | Some namespace -> Ok (Id.block_size namespace)
  | None -> (
    match identify_namespace device ~nsid with
    | Error error -> Error error
    | Ok namespace -> Ok (Id.block_size namespace))

let create_io_queue ?(depth = 32) ?(slot_bytes = default_slot_bytes) device =
  if device.next_qid > max_queues then Error (Bad_argument "No free queue identifiers")
  else if depth < 2 then Error (Bad_argument "Queue depth must be at least 2")
  else
    let qid = device.next_qid in
    let allocate_code = Ffi.io_queue_alloc device.handle qid depth in
    if allocate_code <> Ffi.ok then Error (driver_error device.handle allocate_code)
    else
      match Ffi.io_queue_addresses device.handle qid with
      | Error message -> Error (Driver { code = Ffi.State; message })
      | Ok (sq_iova, cq_iova) -> (
        match Ffi.dma_alloc device.handle (depth * slot_bytes) with
        | Error message -> Error (Driver { code = Ffi.No_memory; message })
        | Ok data_buffer ->
          let vector = if device.interrupt_vectors > qid then qid else 0 in
          let interrupts = device.interrupt_vectors > 0 in
          let cq_command =
            Cmd.create_io_completion_queue ~cid:0 ~qid ~depth ~iova:cq_iova ~vector
              ~interrupts
          in
          let finish () =
            let sq_command =
              Cmd.create_io_submission_queue ~cid:0 ~qid ~depth ~cqid:qid ~iova:sq_iova
            in
            match execute device device.admin sq_command with
            | Error error -> Error error
            | Ok _ ->
              let activate = Ffi.io_queue_activate device.handle qid in
              if activate <> Ffi.ok then Error (driver_error device.handle activate)
              else begin
                let queue = make_queue ~qid ~depth ~vector ~data_buffer ~slot_bytes in
                if interrupts then
                  queue.interrupt <- Ffi.irq_file_descr device.handle vector;
                device.io_queues <- device.io_queues @ [ queue ];
                device.next_qid <- qid + 1;
                Ok queue
              end
          in
          (match execute device device.admin cq_command with
           | Error error ->
             Ffi.dma_free device.handle data_buffer;
             ignore (Ffi.io_queue_free device.handle qid);
             Error error
           | Ok _ -> (
             match finish () with
             | Error error ->
               Ffi.dma_free device.handle data_buffer;
               ignore (Ffi.io_queue_free device.handle qid);
               Error error
             | Ok queue -> Ok queue)))

let delete_io_queue device queue =
  if not queue.live then Error (Bad_argument "Queue already deleted")
  else
    let delete_sq = Cmd.delete_io_submission_queue ~cid:0 ~qid:queue.qid in
    match execute device device.admin delete_sq with
    | Error error -> Error error
    | Ok _ -> (
      let delete_cq = Cmd.delete_io_completion_queue ~cid:0 ~qid:queue.qid in
      match execute device device.admin delete_cq with
      | Error error -> Error error
      | Ok _ ->
        Ffi.dma_free device.handle queue.data_buffer;
        ignore (Ffi.io_queue_free device.handle queue.qid);
        queue.live <- false;
        device.io_queues <- List.filter (fun q -> q.qid <> queue.qid) device.io_queues;
        Ok ())

let slot_offset queue tag = tag * queue.slot_bytes

let prepare_transfer device queue tag sqe length =
  let offset = slot_offset queue tag in
  let code =
    Ffi.prp_build device.handle queue.qid tag queue.data_buffer offset length sqe
  in
  ffi_result device.handle code ()

let submit_transfer ?payload device queue sqe length =
  match acquire_tag queue with
  | None -> Error (Driver { code = Ffi.Queue_full; message = "No free command identifiers" })
  | Some tag -> (
    Cmd.set_opcode_cid sqe
      ~opcode:(Int32.to_int (Cmd.get_dword sqe 0) land 0xff)
      ~cid:tag;
    let staged =
      match payload with
      | None -> Ok ()
      | Some bytes ->
        let code =
          Ffi.dma_write device.handle queue.data_buffer (slot_offset queue tag) bytes 0
            (Bytes.length bytes)
        in
        ffi_result device.handle code ()
    in
    match
      match staged with
      | Error error -> Error error
      | Ok () -> prepare_transfer device queue tag sqe length
    with
    | Error error ->
      release_tag queue tag;
      Error error
    | Ok () ->
      let code = Ffi.io_submit device.handle queue.qid sqe in
      if code <> Ffi.ok then begin
        release_tag queue tag;
        Error (driver_error device.handle code)
      end
      else begin
        let request =
          {
            command_id = tag;
            submitted_at = Unix.gettimeofday ();
            completed_at = 0.0;
            outcome = None;
            result_dword = 0l;
          }
        in
        Hashtbl.replace queue.outstanding tag request;
        Ok request
      end)

let complete_transfer device queue request timeout_ms =
  let deadline = Unix.gettimeofday () +. (float_of_int timeout_ms /. 1000.0) in
  match wait_for device queue request deadline with
  | Error error -> Error error
  | Ok outcome ->
    if Status.is_success outcome.Status.status then Ok request
    else Error (Controller { status = outcome; command_id = request.command_id })

let write_blocks ?(timeout_ms = 5000) device queue ~nsid ~lba ~data =
  match block_size_of device ~nsid with
  | Error error -> Error error
  | Ok block_bytes ->
    let length = Bytes.length data in
    if length = 0 || length mod block_bytes <> 0 then
      Error (Bad_argument (Printf.sprintf "Payload must be a multiple of %d bytes" block_bytes))
    else if length > queue.slot_bytes then
      Error (Bad_argument (Printf.sprintf "Payload exceeds the %d byte transfer slot" queue.slot_bytes))
    else
      let blocks = length / block_bytes in
      let sqe = Cmd.write ~cid:0 ~nsid ~lba ~blocks in
      match submit_transfer ~payload:data device queue sqe length with
      | Error error -> Error error
      | Ok request -> (
        match complete_transfer device queue request timeout_ms with
        | Error error -> Error error
        | Ok _ -> Ok ())

let read_blocks ?(timeout_ms = 5000) device queue ~nsid ~lba ~blocks =
  match block_size_of device ~nsid with
  | Error error -> Error error
  | Ok block_bytes ->
    let length = blocks * block_bytes in
    if blocks <= 0 then Error (Bad_argument "Block count must be positive")
    else if length > queue.slot_bytes then
      Error (Bad_argument (Printf.sprintf "Transfer exceeds the %d byte slot" queue.slot_bytes))
    else
      let sqe = Cmd.read ~cid:0 ~nsid ~lba ~blocks in
      match submit_transfer device queue sqe length with
      | Error error -> Error error
      | Ok request -> (
        match complete_transfer device queue request timeout_ms with
        | Error error -> Error error
        | Ok request ->
          let buffer = Bytes.create length in
          let offset = slot_offset queue request.command_id in
          let code = Ffi.dma_read device.handle queue.data_buffer offset buffer 0 length in
          if code = Ffi.ok then Ok buffer else Error (driver_error device.handle code))

let flush ?(timeout_ms = 5000) device queue ~nsid =
  let sqe = Cmd.flush ~cid:0 ~nsid in
  match submit device queue sqe with
  | Error error -> Error error
  | Ok request -> (
    match complete_transfer device queue request timeout_ms with
    | Error error -> Error error
    | Ok _ -> Ok ())

let write_zeroes ?(timeout_ms = 5000) device queue ~nsid ~lba ~blocks =
  let sqe = Cmd.write_zeroes ~cid:0 ~nsid ~lba ~blocks in
  match submit device queue sqe with
  | Error error -> Error error
  | Ok request -> (
    match complete_transfer device queue request timeout_ms with
    | Error error -> Error error
    | Ok _ -> Ok ())

let create_namespace device ~blocks ~lba_format =
  let payload = Id.encode_namespace_request ~blocks ~lba_format_index:lba_format in
  match write_scratch device payload with
  | Error error -> Error error
  | Ok () -> (
    let sqe = Cmd.namespace_create ~cid:0 ~iova:(scratch_iova device) in
    match execute device device.admin sqe with
    | Error error -> Error error
    | Ok request -> Ok (Int32.to_int request.result_dword))

let delete_namespace device ~nsid =
  let sqe = Cmd.namespace_delete ~cid:0 ~nsid in
  match execute device device.admin sqe with
  | Error error -> Error error
  | Ok _ ->
    Hashtbl.remove device.namespace_cache nsid;
    Ok ()

let attach_namespace device ~nsid =
  let payload = Id.encode_controller_list [ 1 ] in
  match write_scratch device payload with
  | Error error -> Error error
  | Ok () -> (
    let sqe = Cmd.namespace_attach ~cid:0 ~nsid ~iova:(scratch_iova device) in
    match execute device device.admin sqe with
    | Error error -> Error error
    | Ok _ -> Ok ())

let detach_namespace device ~nsid =
  let payload = Id.encode_controller_list [ 1 ] in
  match write_scratch device payload with
  | Error error -> Error error
  | Ok () -> (
    let sqe = Cmd.namespace_detach ~cid:0 ~nsid ~iova:(scratch_iova device) in
    match execute device device.admin sqe with
    | Error error -> Error error
    | Ok _ ->
      Hashtbl.remove device.namespace_cache nsid;
      Ok ())

let reset device =
  let code = Ffi.dev_reset device.handle in
  if code <> Ffi.ok then Error (driver_error device.handle code)
  else begin
    List.iter (fun queue -> queue.live <- false) device.io_queues;
    device.io_queues <- [];
    device.next_qid <- 1;
    Hashtbl.reset device.admin.outstanding;
    Stack.clear device.admin.free_tags;
    for tag = device.admin.depth - 1 downto 0 do
      Stack.push tag device.admin.free_tags
    done;
    Hashtbl.reset device.namespace_cache;
    Ok ()
  end

module Pipeline = struct
  type slot = {
    request : request;
    length : int;
    tag : int;
  }

  let outstanding queue = inflight queue

  let capacity queue = queue.depth - 1

  let submit_read device queue ~nsid ~lba ~blocks ~block_bytes =
    let length = blocks * block_bytes in
    if length > queue.slot_bytes then
      Error (Bad_argument "Transfer exceeds slot size")
    else
      let sqe = Cmd.read ~cid:0 ~nsid ~lba ~blocks in
      match submit_transfer device queue sqe length with
      | Error error -> Error error
      | Ok request -> Ok { request; length; tag = request.command_id }

  let submit_write ?payload device queue ~nsid ~lba ~blocks ~block_bytes =
    let length = blocks * block_bytes in
    if length > queue.slot_bytes then
      Error (Bad_argument "Transfer exceeds slot size")
    else
      let sqe = Cmd.write ~cid:0 ~nsid ~lba ~blocks in
      match submit_transfer ?payload device queue sqe length with
      | Error error -> Error error
      | Ok request -> Ok { request; length; tag = request.command_id }

  let reap device queue timeout_ms =
    match reap_once device queue timeout_ms with
    | Error error -> Error error
    | Ok None -> Ok None
    | Ok (Some request) -> (
      match request.outcome with
      | Some outcome when Status.is_success outcome.Status.status ->
        Ok (Some (request, request.completed_at -. request.submitted_at))
      | Some outcome -> Error (Controller { status = outcome; command_id = request.command_id })
      | None -> Error (Bad_argument "Completion without status"))

  let slot_request slot = slot.request
  let slot_command_id slot = slot.tag
  let slot_length slot = slot.length

  let read_slot device queue slot =
    let buffer = Bytes.create slot.length in
    let code =
      Ffi.dma_read device.handle queue.data_buffer (slot_offset queue slot.tag) buffer 0
        slot.length
    in
    if code = Ffi.ok then Ok buffer else Error (driver_error device.handle code)
end

let request_command_id request = request.command_id
let request_status request = request.outcome
let request_result_dword request = request.result_dword

let request_latency request =
  if request.completed_at > 0.0 then request.completed_at -. request.submitted_at else 0.0

let queue_id queue = queue.qid
let queue_depth queue = queue.depth
let queue_vector queue = queue.vector
let queue_outstanding queue = inflight queue
let queue_slot_bytes queue = queue.slot_bytes
let admin_queue device = device.admin
let io_queues device = device.io_queues
