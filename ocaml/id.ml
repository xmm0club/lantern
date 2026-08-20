type lba_format = {
  metadata_size : int;
  data_size : int;
  relative_performance : int;
}

type controller = {
  vendor_id : int;
  subsystem_vendor_id : int;
  serial : string;
  model : string;
  firmware : string;
  ieee : string;
  max_transfer_shift : int;
  controller_id : int;
  version : int32;
  optional_admin_commands : int;
  optional_nvm_commands : int;
  submission_entry_size : int;
  completion_entry_size : int;
  max_outstanding : int;
  namespace_count : int;
  volatile_write_cache : bool;
  warning_temperature : int;
  critical_temperature : int;
}

type namespace = {
  size_blocks : int64;
  capacity_blocks : int64;
  used_blocks : int64;
  formats : lba_format array;
  format_index : int;
  namespace_capacity_bytes : int64;
  globally_unique_identifier : string;
}

type smart = {
  critical_warning : int;
  composite_temperature_kelvin : int;
  available_spare : int;
  percentage_used : int;
  data_units_read : int64;
  data_units_written : int64;
  host_read_commands : int64;
  host_write_commands : int64;
}

let trimmed buffer offset length =
  let raw = Bytes.sub_string buffer offset length in
  String.trim raw

let hex_string buffer offset length =
  String.concat ""
    (List.init length (fun i ->
         Printf.sprintf "%02x" (Char.code (Bytes.get buffer (offset + i)))))

let uint16 buffer offset = Char.code (Bytes.get buffer offset)
                           lor (Char.code (Bytes.get buffer (offset + 1)) lsl 8)

let uint8 buffer offset = Char.code (Bytes.get buffer offset)

let uint32 buffer offset =
  Int64.to_int (Int64.logand (Int64.of_int32 (Bytes.get_int32_le buffer offset)) 0xffffffffL)

let parse_controller buffer = {
  vendor_id = uint16 buffer 0;
  subsystem_vendor_id = uint16 buffer 2;
  serial = trimmed buffer 4 20;
  model = trimmed buffer 24 40;
  firmware = trimmed buffer 64 8;
  ieee = hex_string buffer 73 3;
  max_transfer_shift = uint8 buffer 77;
  controller_id = uint16 buffer 78;
  version = Bytes.get_int32_le buffer 80;
  optional_admin_commands = uint16 buffer 256;
  warning_temperature = uint16 buffer 266;
  critical_temperature = uint16 buffer 268;
  submission_entry_size = uint8 buffer 512;
  completion_entry_size = uint8 buffer 513;
  max_outstanding = uint16 buffer 514;
  namespace_count = uint32 buffer 516;
  optional_nvm_commands = uint16 buffer 520;
  volatile_write_cache = uint8 buffer 525 land 1 = 1;
}

let parse_namespace buffer =
  let format_count = uint8 buffer 25 + 1 in
  let formats =
    Array.init format_count (fun i ->
        let offset = 128 + (i * 4) in
        {
          metadata_size = uint16 buffer offset;
          data_size = 1 lsl uint8 buffer (offset + 2);
          relative_performance = uint8 buffer (offset + 3) land 0x3;
        })
  in
  {
    size_blocks = Bytes.get_int64_le buffer 0;
    capacity_blocks = Bytes.get_int64_le buffer 8;
    used_blocks = Bytes.get_int64_le buffer 16;
    formats;
    format_index = uint8 buffer 26 land 0xf;
    namespace_capacity_bytes = Bytes.get_int64_le buffer 48;
    globally_unique_identifier = hex_string buffer 104 16;
  }

let block_size namespace =
  if namespace.format_index < Array.length namespace.formats then
    namespace.formats.(namespace.format_index).data_size
  else 512

let parse_smart buffer = {
  critical_warning = uint8 buffer 0;
  available_spare = uint8 buffer 3;
  composite_temperature_kelvin = uint16 buffer 1;
  percentage_used = uint8 buffer 5;
  data_units_read = Bytes.get_int64_le buffer 32;
  data_units_written = Bytes.get_int64_le buffer 48;
  host_read_commands = Bytes.get_int64_le buffer 64;
  host_write_commands = Bytes.get_int64_le buffer 80;
}

let encode_namespace_request ~blocks ~lba_format_index =
  let buffer = Bytes.make 4096 '\000' in
  Bytes.set_int64_le buffer 0 blocks;
  Bytes.set_int64_le buffer 8 blocks;
  Bytes.set buffer 26 (Char.chr (lba_format_index land 0xf));
  buffer

let encode_controller_list ids =
  let buffer = Bytes.make 4096 '\000' in
  Bytes.set_int16_le buffer 0 (List.length ids);
  List.iteri (fun i id -> Bytes.set_int16_le buffer (2 + (i * 2)) id) ids;
  buffer
