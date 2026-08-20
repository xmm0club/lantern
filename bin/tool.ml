let exit_usage = 2
let exit_failure = 1

type common = {
  bdf : string;
  transport : Nvme.transport;
  image : string;
  capacity : int64;
  block_size : int;
  namespaces : int;
  admin_depth : int;
  interrupts : bool;
  latency_us : int;
  nsid : int;
  queue_depth : int;
}

let parse_size text =
  let length = String.length text in
  if length = 0 then None
  else
    let suffix = Char.lowercase_ascii text.[length - 1] in
    let multiplier =
      match suffix with
      | 'k' -> Some 1024L
      | 'm' -> Some (Int64.mul 1024L 1024L)
      | 'g' -> Some (Int64.mul 1024L (Int64.mul 1024L 1024L))
      | _ -> None
    in
    match multiplier with
    | None -> Int64.of_string_opt text
    | Some factor -> (
      match Int64.of_string_opt (String.sub text 0 (length - 1)) with
      | None -> None
      | Some value -> Some (Int64.mul value factor))

let human_bytes value =
  let value = Int64.to_float value in
  if value >= 1024.0 *. 1024.0 *. 1024.0 then
    Printf.sprintf "%.2f GiB" (value /. (1024.0 *. 1024.0 *. 1024.0))
  else if value >= 1024.0 *. 1024.0 then Printf.sprintf "%.2f MiB" (value /. (1024.0 *. 1024.0))
  else if value >= 1024.0 then Printf.sprintf "%.2f KiB" (value /. 1024.0)
  else Printf.sprintf "%.0f B" value

let config_of_common common =
  {
    Nvme.transport = common.transport;
    admin_queue_depth = common.admin_depth;
    use_interrupts = common.interrupts;
    image_path = common.image;
    capacity_bytes = common.capacity;
    block_size = common.block_size;
    namespace_count = common.namespaces;
    latency_us = common.latency_us;
    serial = "DEADBEEF";
    model = "lantern mock nvme";
  }

let report_error error =
  prerr_endline ("nvme-tool: " ^ Nvme.string_of_error error);
  exit_failure

let with_device common body =
  match Nvme.open_device ~config:(config_of_common common) common.bdf with
  | Error error -> report_error error
  | Ok device ->
    let status = try body device with exn -> Nvme.close device; raise exn in
    Nvme.close device;
    status

let ( let* ) result f = match result with Error error -> Error error | Ok value -> f value

let print_registers device =
  let state = Nvme.registers device in
  Printf.printf "Controller registers\n";
  Printf.printf "  CAP   0x%016Lx  MQES %d  DSTRD %d  timeout %d ms\n" state.Ffi.cap
    state.Ffi.max_queue_entries state.Ffi.doorbell_stride state.Ffi.timeout_ms;
  Printf.printf "  VS    %d.%d.%d\n"
    (Int32.to_int (Int32.shift_right_logical state.Ffi.vs 16) land 0xffff)
    (Int32.to_int (Int32.shift_right_logical state.Ffi.vs 8) land 0xff)
    (Int32.to_int state.Ffi.vs land 0xff);
  Printf.printf "  CC    0x%08lx      CSTS 0x%08lx      AQA 0x%08lx\n" state.Ffi.cc
    state.Ffi.csts state.Ffi.aqa;
  Printf.printf "  ASQ   0x%016Lx  ACQ  0x%016Lx\n" state.Ffi.asq state.Ffi.acq

let print_queue_state device qid label =
  let state = Nvme.queue_registers device qid in
  Printf.printf "  Queue %-5s depth %4d  SQ tail %4d  CQ head %4d  phase %d  submitted %Ld  completed %Ld\n"
    label state.Ffi.depth state.Ffi.sq_tail state.Ffi.cq_head
    state.Ffi.cq_phase state.Ffi.submitted state.Ffi.completed

let identify_command common verbose =
  with_device common (fun device ->
      let outcome =
        let* controller = Nvme.identify_controller device in
        let* namespaces = Nvme.active_namespaces device in
        let* smart = Nvme.smart_log device in
        Printf.printf "Device %s via %s\n" (Nvme.bdf device) (Nvme.backend_name device);
        Printf.printf "Controller\n";
        Printf.printf "  Model            %s\n" controller.Id.model;
        Printf.printf "  Serial           %s\n" controller.Id.serial;
        Printf.printf "  Firmware         %s\n" controller.Id.firmware;
        Printf.printf "  PCI vendor       0x%04x  subsystem 0x%04x  IEEE %s\n"
          controller.Id.vendor_id controller.Id.subsystem_vendor_id
          controller.Id.ieee;
        Printf.printf "  NVMe version     %d.%d.%d\n"
          (Int32.to_int (Int32.shift_right_logical controller.Id.version 16) land 0xffff)
          (Int32.to_int (Int32.shift_right_logical controller.Id.version 8) land 0xff)
          (Int32.to_int controller.Id.version land 0xff);
        Printf.printf "  Controller ID    %d\n" controller.Id.controller_id;
        Printf.printf "  Max transfer     %s (MDTS %d)\n"
          (human_bytes (Int64.of_int (4096 lsl controller.Id.max_transfer_shift)))
          controller.Id.max_transfer_shift;
        Printf.printf "  Namespaces       %d\n" controller.Id.namespace_count;
        Printf.printf "  Volatile cache   %s\n"
          (if controller.Id.volatile_write_cache then "present" else "absent");
        Printf.printf "  SQEs/CQEs        0x%02x / 0x%02x\n" controller.Id.submission_entry_size
          controller.Id.completion_entry_size;
        List.iter
          (fun nsid ->
            match Nvme.identify_namespace device ~nsid with
            | Error error -> Printf.printf "Namespace %d: %s\n" nsid (Nvme.string_of_error error)
            | Ok namespace ->
              let block = Id.block_size namespace in
              Printf.printf "Namespace %d\n" nsid;
              Printf.printf "  Blocks           %Ld of %d bytes (%s)\n"
                namespace.Id.size_blocks block
                (human_bytes (Int64.mul namespace.Id.size_blocks (Int64.of_int block)));
              Printf.printf "  Utilization      %Ld blocks\n" namespace.Id.used_blocks;
              Printf.printf "  GUID             %s\n"
                namespace.Id.globally_unique_identifier;
              Array.iteri
                (fun index format ->
                  Printf.printf "  LBA format %d     %d bytes%s\n" index
                    format.Id.data_size
                    (if index = namespace.Id.format_index then "  (in use)" else ""))
                namespace.Id.formats)
          namespaces;
        Printf.printf "Health\n";
        Printf.printf "  Temperature      %d K\n" smart.Id.composite_temperature_kelvin;
        Printf.printf "  Available spare  %d%%\n" smart.Id.available_spare;
        Printf.printf "  Data read        %s\n"
          (human_bytes (Int64.mul smart.Id.data_units_read 512L));
        Printf.printf "  Data written     %s\n"
          (human_bytes (Int64.mul smart.Id.data_units_written 512L));
        if verbose then begin
          print_registers device;
          print_queue_state device 0 "admin"
        end;
        Ok ()
      in
      match outcome with Error error -> report_error error | Ok () -> 0)

let chunk_blocks common queue block_bytes =
  let slot = Nvme.queue_slot_bytes queue in
  max 1 (min (slot / block_bytes) (128 * 1024 / block_bytes) * 1)
  |> fun value -> if value = 0 then 1 else min value (max 1 common.queue_depth * 8)

let read_command common lba count output =
  with_device common (fun device ->
      let outcome =
        let* queue = Nvme.create_io_queue ~depth:common.queue_depth device in
        let* namespace = Nvme.identify_namespace device ~nsid:common.nsid in
        let block_bytes = Id.block_size namespace in
        let per_command = chunk_blocks common queue block_bytes in
        let channel = open_out_bin output in
        let rec loop offset remaining =
          if remaining = 0 then Ok ()
          else
            let blocks = min remaining per_command in
            match
              Nvme.read_blocks device queue ~nsid:common.nsid
                ~lba:(Int64.add lba (Int64.of_int offset))
                ~blocks
            with
            | Error error -> Error error
            | Ok payload ->
              output_bytes channel payload;
              loop (offset + blocks) (remaining - blocks)
        in
        let result = loop 0 count in
        close_out channel;
        let* () = result in
        Printf.printf "Read %d blocks of %d bytes from LBA %Ld into %s\n" count block_bytes lba
          output;
        print_queue_state device (Nvme.queue_id queue) (string_of_int (Nvme.queue_id queue));
        let* () = Nvme.delete_io_queue device queue in
        Ok ()
      in
      match outcome with Error error -> report_error error | Ok () -> 0)

let write_command common lba count input =
  with_device common (fun device ->
      let outcome =
        let* queue = Nvme.create_io_queue ~depth:common.queue_depth device in
        let* namespace = Nvme.identify_namespace device ~nsid:common.nsid in
        let block_bytes = Id.block_size namespace in
        let per_command = chunk_blocks common queue block_bytes in
        let channel = open_in_bin input in
        let available = in_channel_length channel in
        let needed = count * block_bytes in
        if available < needed then begin
          close_in channel;
          Error
            (Nvme.Bad_argument
               (Printf.sprintf "%s holds %d bytes, %d needed" input available needed))
        end
        else
          let rec loop offset remaining =
            if remaining = 0 then Ok ()
            else
              let blocks = min remaining per_command in
              let payload = Bytes.create (blocks * block_bytes) in
              really_input channel payload 0 (Bytes.length payload);
              match
                Nvme.write_blocks device queue ~nsid:common.nsid
                  ~lba:(Int64.add lba (Int64.of_int offset))
                  ~data:payload
              with
              | Error error -> Error error
              | Ok () -> loop (offset + blocks) (remaining - blocks)
          in
          let result = loop 0 count in
          close_in channel;
          let* () = result in
          let* () = Nvme.flush device queue ~nsid:common.nsid in
          Printf.printf "Wrote %d blocks of %d bytes to LBA %Ld from %s\n" count block_bytes lba
            input;
          print_queue_state device (Nvme.queue_id queue) (string_of_int (Nvme.queue_id queue));
          let* () = Nvme.delete_io_queue device queue in
          Ok ()
      in
      match outcome with Error error -> report_error error | Ok () -> 0)

module Histogram = struct
  let buckets = 24

  type t = { counts : int array; mutable total : int; mutable sum : float; mutable worst : float }

  let create () = { counts = Array.make buckets 0; total = 0; sum = 0.0; worst = 0.0 }

  let bucket_of_micros micros =
    if micros <= 1.0 then 0
    else
      let index = int_of_float (ceil (log micros /. log 2.0)) in
      if index >= buckets then buckets - 1 else index

  let add histogram seconds =
    let micros = seconds *. 1_000_000.0 in
    histogram.counts.(bucket_of_micros micros) <- histogram.counts.(bucket_of_micros micros) + 1;
    histogram.total <- histogram.total + 1;
    histogram.sum <- histogram.sum +. micros;
    if micros > histogram.worst then histogram.worst <- micros

  let percentile histogram fraction =
    let target = int_of_float (ceil (float_of_int histogram.total *. fraction)) in
    let rec walk index seen =
      if index >= buckets then float_of_int (1 lsl (buckets - 1))
      else
        let seen = seen + histogram.counts.(index) in
        if seen >= target then float_of_int (1 lsl index) else walk (index + 1) seen
    in
    walk 0 0

  let print histogram =
    let peak = Array.fold_left max 1 histogram.counts in
    Printf.printf "Latency distribution\n";
    Array.iteri
      (fun index count ->
        if count > 0 then begin
          let width = count * 40 / peak in
          Printf.printf "  %7s  %7d  %s\n"
            (if index = 0 then "<1us" else Printf.sprintf "<%dus" (1 lsl index))
            count
            (String.make (max 1 width) '#')
        end)
      histogram.counts
end

let bench_command common pattern total_size io_size queues seed operation =
  with_device common (fun device ->
      let outcome =
        let* namespace = Nvme.identify_namespace device ~nsid:common.nsid in
        let block_bytes = Id.block_size namespace in
        if io_size mod block_bytes <> 0 then
          Error
            (Nvme.Bad_argument
               (Printf.sprintf "I/O size %d is not a multiple of the %d byte block" io_size
                  block_bytes))
        else
          let blocks_per_io = io_size / block_bytes in
          let total_ios = Int64.to_int (Int64.div total_size (Int64.of_int io_size)) in
          if total_ios <= 0 then Error (Nvme.Bad_argument "Transfer size is smaller than one I/O")
          else
            let rec build index acc =
              if index >= queues then Ok (List.rev acc)
              else
                match Nvme.create_io_queue ~depth:common.queue_depth device with
                | Error error -> Error error
                | Ok queue -> build (index + 1) (queue :: acc)
            in
            let* queue_list = build 0 [] in
            let queue_array = Array.of_list queue_list in
            let namespace_blocks = namespace.Id.size_blocks in
            let random = Random.State.make [| seed |] in
            let max_start = Int64.sub namespace_blocks (Int64.of_int blocks_per_io) in
            let lba_of index =
              match pattern with
              | `Sequential ->
                Int64.rem
                  (Int64.mul (Int64.of_int index) (Int64.of_int blocks_per_io))
                  (Int64.max 1L max_start)
              | `Random ->
                let span = Int64.to_float max_start in
                let choice = Random.State.float random span in
                let raw = Int64.of_float choice in
                Int64.mul (Int64.div raw (Int64.of_int blocks_per_io)) (Int64.of_int blocks_per_io)
            in
            let payload = Bytes.init io_size (fun i -> Char.chr ((i * 7) land 0xff)) in
            let histogram = Histogram.create () in
            let submitted = ref 0 in
            let completed = ref 0 in
            let failure = ref None in
            let start = Unix.gettimeofday () in
            let submit_one () =
              let index = !submitted in
              let queue = queue_array.(index mod Array.length queue_array) in
              let lba = lba_of index in
              let result =
                match operation with
                | `Read ->
                  Nvme.Pipeline.submit_read device queue ~nsid:common.nsid ~lba
                    ~blocks:blocks_per_io ~block_bytes
                | `Write ->
                  Nvme.Pipeline.submit_write ~payload device queue ~nsid:common.nsid ~lba
                    ~blocks:blocks_per_io ~block_bytes
              in
              match result with
              | Error (Nvme.Driver { code; _ }) when code = Ffi.err_queue_full -> false
              | Error error ->
                failure := Some error;
                false
              | Ok _ ->
                incr submitted;
                true
            in
            let reap_any blocking =
              let reaped = ref false in
              Array.iter
                (fun queue ->
                  if not !reaped && Nvme.queue_outstanding queue > 0 then
                    match Nvme.Pipeline.reap device queue (if blocking then 1 else 0) with
                    | Error error -> failure := Some error
                    | Ok None -> ()
                    | Ok (Some (_, latency)) ->
                      Histogram.add histogram latency;
                      incr completed;
                      reaped := true)
                queue_array;
              !reaped
            in
            while !failure = None && !completed < total_ios do
              let mutated = ref false in
              while
                !failure = None && !submitted < total_ios
                && Array.exists (fun q -> Nvme.queue_outstanding q < common.queue_depth - 1)
                     queue_array
                && submit_one ()
              do
                mutated := true
              done;
              if !failure = None && not (reap_any (not !mutated)) then ignore (reap_any true)
            done;
            let elapsed = Unix.gettimeofday () -. start in
            (match !failure with
             | Some error -> Error error
             | None ->
               let bytes = Int64.mul (Int64.of_int !completed) (Int64.of_int io_size) in
               Printf.printf "Benchmark\n";
               Printf.printf "  Pattern          %s %s\n"
                 (match pattern with `Sequential -> "Sequential" | `Random -> "Random")
                 (match operation with `Read -> "read" | `Write -> "write");
               Printf.printf "  I/O size         %d bytes (%d blocks)\n" io_size blocks_per_io;
               Printf.printf "  Queues           %d, depth %d%s\n" queues common.queue_depth
                 (if Nvme.interrupts_enabled device then ", MSI-X completion signaling"
                  else ", polled completion");
               Printf.printf "  Transferred      %s in %.3f s\n" (human_bytes bytes) elapsed;
               Printf.printf "  Throughput       %.2f MiB/s\n"
                 (Int64.to_float bytes /. elapsed /. 1024.0 /. 1024.0);
               Printf.printf "  IOPS             %.0f\n" (float_of_int !completed /. elapsed);
               Printf.printf "  Latency mean     %.1f us\n"
                 (histogram.Histogram.sum /. float_of_int (max 1 histogram.Histogram.total));
               Printf.printf "  Latency p50      < %.0f us\n" (Histogram.percentile histogram 0.50);
               Printf.printf "  Latency p99      < %.0f us\n" (Histogram.percentile histogram 0.99);
               Printf.printf "  Latency max      %.1f us\n" histogram.Histogram.worst;
               Histogram.print histogram;
               Array.iter
                 (fun queue ->
                   print_queue_state device (Nvme.queue_id queue)
                     (string_of_int (Nvme.queue_id queue)))
                 queue_array;
               List.fold_left
                 (fun acc queue -> match acc with Error e -> Error e | Ok () -> Nvme.delete_io_queue device queue)
                 (Ok ()) queue_list)
      in
      match outcome with Error error -> report_error error | Ok () -> 0)

open Cmdliner

let bdf_argument =
  let doc = "PCI address of the controller, for example 0000:00:04.0." in
  Arg.(required & pos 0 (some string) None & info [] ~docv:"BDF" ~doc)

let transport_conv =
  let parse = function
    | "vfio" -> Ok Nvme.Vfio
    | "mock" -> Ok Nvme.Mock
    | other -> Error (`Msg (Printf.sprintf "Unknown transport %s" other))
  in
  let print formatter value =
    Format.fprintf formatter "%s" (match value with Nvme.Vfio -> "vfio" | Nvme.Mock -> "mock")
  in
  Arg.conv (parse, print)

let size_conv =
  let parse text =
    match parse_size text with
    | Some value -> Ok value
    | None -> Error (`Msg (Printf.sprintf "Cannot parse size %s" text))
  in
  let print formatter value = Format.fprintf formatter "%Ld" value in
  Arg.conv (parse, print)

let common_term =
  let transport =
    let doc = "Backend to drive: vfio for real hardware, mock for the simulated controller." in
    Arg.(value & opt transport_conv Nvme.Mock & info [ "transport" ] ~docv:"KIND" ~doc)
  in
  let image =
    let doc = "Backing file used by the mock controller." in
    Arg.(value & opt string "mock.img" & info [ "image" ] ~docv:"PATH" ~doc)
  in
  let capacity =
    let doc = "Total capacity presented by the mock controller." in
    Arg.(value & opt size_conv (Int64.of_int (256 * 1024 * 1024)) & info [ "capacity" ] ~docv:"SIZE" ~doc)
  in
  let block_size =
    let doc = "Logical block size presented by the mock controller." in
    Arg.(value & opt int 512 & info [ "block-size" ] ~docv:"BYTES" ~doc)
  in
  let namespaces =
    let doc = "Number of namespace slots on the mock controller." in
    Arg.(value & opt int 4 & info [ "namespaces" ] ~docv:"COUNT" ~doc)
  in
  let admin_depth =
    let doc = "Admin queue depth." in
    Arg.(value & opt int 64 & info [ "admin-depth" ] ~docv:"ENTRIES" ~doc)
  in
  let interrupts =
    let doc = "Use MSI-X eventfd completion signaling instead of pure polling." in
    Arg.(value & flag & info [ "interrupts" ] ~doc)
  in
  let latency =
    let doc = "Artificial per command service time for the mock controller." in
    Arg.(value & opt int 0 & info [ "latency-us" ] ~docv:"MICROSECONDS" ~doc)
  in
  let nsid =
    let doc = "Namespace identifier to operate on." in
    Arg.(value & opt int 1 & info [ "nsid" ] ~docv:"ID" ~doc)
  in
  let queue_depth =
    let doc = "Depth of each io queue pair." in
    Arg.(value & opt int 32 & info [ "queue-depth" ] ~docv:"ENTRIES" ~doc)
  in
  let build bdf transport image capacity block_size namespaces admin_depth interrupts latency_us
      nsid queue_depth =
    { bdf; transport; image; capacity; block_size; namespaces; admin_depth; interrupts;
      latency_us; nsid; queue_depth }
  in
  Term.(
    const build $ bdf_argument $ transport $ image $ capacity $ block_size $ namespaces
    $ admin_depth $ interrupts $ latency $ nsid $ queue_depth)

let identify_cmd =
  let verbose =
    let doc = "Also print controller registers and admin queue state." in
    Arg.(value & flag & info [ "v"; "verbose" ] ~doc)
  in
  let term = Term.(const identify_command $ common_term $ verbose) in
  let info = Cmd.info "identify" ~doc:"Print controller, namespace and health information." in
  Cmd.v info term

let read_cmd =
  let lba =
    let doc = "First logical block address." in
    Arg.(required & pos 1 (some int64) None & info [] ~docv:"LBA" ~doc)
  in
  let count =
    let doc = "Number of logical blocks to read." in
    Arg.(required & pos 2 (some int) None & info [] ~docv:"COUNT" ~doc)
  in
  let output =
    let doc = "File that receives the data." in
    Arg.(required & opt (some string) None & info [ "o"; "output" ] ~docv:"FILE" ~doc)
  in
  let term = Term.(const read_command $ common_term $ lba $ count $ output) in
  let info = Cmd.info "read" ~doc:"Read logical blocks into a file." in
  Cmd.v info term

let write_cmd =
  let lba =
    let doc = "First logical block address." in
    Arg.(required & pos 1 (some int64) None & info [] ~docv:"LBA" ~doc)
  in
  let count =
    let doc = "Number of logical blocks to write." in
    Arg.(required & pos 2 (some int) None & info [] ~docv:"COUNT" ~doc)
  in
  let input =
    let doc = "File holding the data to write." in
    Arg.(required & opt (some string) None & info [ "i"; "input" ] ~docv:"FILE" ~doc)
  in
  let term = Term.(const write_command $ common_term $ lba $ count $ input) in
  let info = Cmd.info "write" ~doc:"Write logical blocks from a file." in
  Cmd.v info term

let bench_cmd =
  let pattern =
    let doc = "Access pattern: seq or rand." in
    let pattern_conv =
      Arg.conv
        ( (function
          | "seq" -> Ok `Sequential
          | "rand" -> Ok `Random
          | other -> Error (`Msg (Printf.sprintf "Unknown pattern %s" other))),
          fun formatter value ->
            Format.fprintf formatter "%s"
              (match value with `Sequential -> "seq" | `Random -> "rand") )
    in
    Arg.(value & opt pattern_conv `Sequential & info [ "pattern" ] ~docv:"KIND" ~doc)
  in
  let operation =
    let doc = "Direction: read or write." in
    let operation_conv =
      Arg.conv
        ( (function
          | "read" -> Ok `Read
          | "write" -> Ok `Write
          | other -> Error (`Msg (Printf.sprintf "Unknown operation %s" other))),
          fun formatter value ->
            Format.fprintf formatter "%s" (match value with `Read -> "read" | `Write -> "write") )
    in
    Arg.(value & opt operation_conv `Read & info [ "rw" ] ~docv:"KIND" ~doc)
  in
  let size =
    let doc = "Total number of bytes to transfer." in
    Arg.(value & opt size_conv (Int64.of_int (32 * 1024 * 1024)) & info [ "size" ] ~docv:"SIZE" ~doc)
  in
  let io_size =
    let doc = "Size of each individual command." in
    Arg.(value & opt int 4096 & info [ "io-size" ] ~docv:"BYTES" ~doc)
  in
  let queues =
    let doc = "Number of io queue pairs to spread the work across." in
    Arg.(value & opt int 1 & info [ "queues" ] ~docv:"COUNT" ~doc)
  in
  let seed =
    let doc = "Seed for the random access pattern." in
    Arg.(value & opt int 20250820 & info [ "seed" ] ~docv:"NUMBER" ~doc)
  in
  let term =
    Term.(const bench_command $ common_term $ pattern $ size $ io_size $ queues $ seed $ operation)
  in
  let info = Cmd.info "bench" ~doc:"Measure throughput, iops and latency." in
  Cmd.v info term

let () =
  let info =
    Cmd.info "nvme-tool" ~version:"0.1.0"
      ~doc:"Userspace NVMe control plane driving the lantern vfio driver."
  in
  let group = Cmd.group ~default:Term.(ret (const (`Help (`Pager, None)))) info
      [ identify_cmd; read_cmd; write_cmd; bench_cmd ] in
  exit (Cmd.eval' ~term_err:exit_usage group)
