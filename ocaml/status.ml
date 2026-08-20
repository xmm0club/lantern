type generic =
  | Invalid_opcode
  | Invalid_field
  | Command_id_conflict
  | Data_transfer_error
  | Aborted_power_loss
  | Internal_error
  | Command_abort_requested
  | Abort_sq_deletion
  | Fused_command_failed
  | Fused_command_missing
  | Invalid_namespace_or_format
  | Command_sequence_error
  | Lba_out_of_range
  | Capacity_exceeded
  | Namespace_not_ready
  | Unknown_generic of int

type command_specific =
  | Completion_queue_invalid
  | Invalid_queue_identifier
  | Invalid_queue_size
  | Abort_command_limit_exceeded
  | Invalid_queue_deletion
  | Feature_not_changeable
  | Feature_not_namespace_specific
  | Namespace_identifier_unavailable
  | Namespace_already_attached
  | Namespace_not_attached
  | Unknown_command_specific of int

type media =
  | Write_fault
  | Unrecovered_read_error
  | Guard_check_error
  | Reference_tag_check_error
  | Compare_failure
  | Access_denied
  | Unknown_media of int

type t =
  | Success
  | Generic of generic
  | Command_specific of command_specific
  | Media of media
  | Path_related of int
  | Vendor_specific of int
  | Reserved of int * int

type decoded = {
  status : t;
  status_code : int;
  status_code_type : int;
  do_not_retry : bool;
  more : bool;
  command_retry_delay : int;
}

let generic_of_code = function
  | 0x01 -> Invalid_opcode
  | 0x02 -> Invalid_field
  | 0x03 -> Command_id_conflict
  | 0x04 -> Data_transfer_error
  | 0x05 -> Aborted_power_loss
  | 0x06 -> Internal_error
  | 0x07 -> Command_abort_requested
  | 0x08 -> Abort_sq_deletion
  | 0x09 -> Fused_command_failed
  | 0x0a -> Fused_command_missing
  | 0x0b -> Invalid_namespace_or_format
  | 0x0c -> Command_sequence_error
  | 0x80 -> Lba_out_of_range
  | 0x81 -> Capacity_exceeded
  | 0x82 -> Namespace_not_ready
  | code -> Unknown_generic code

let command_specific_of_code = function
  | 0x00 -> Completion_queue_invalid
  | 0x01 -> Invalid_queue_identifier
  | 0x02 -> Invalid_queue_size
  | 0x03 -> Abort_command_limit_exceeded
  | 0x0c -> Invalid_queue_deletion
  | 0x0e -> Feature_not_changeable
  | 0x0f -> Feature_not_namespace_specific
  | 0x16 -> Namespace_identifier_unavailable
  | 0x18 -> Namespace_already_attached
  | 0x1a -> Namespace_not_attached
  | code -> Unknown_command_specific code

let media_of_code = function
  | 0x80 -> Write_fault
  | 0x81 -> Unrecovered_read_error
  | 0x82 -> Guard_check_error
  | 0x83 -> Reference_tag_check_error
  | 0x85 -> Compare_failure
  | 0x86 -> Access_denied
  | code -> Unknown_media code

let decode word =
  let status_code = word land 0xff in
  let status_code_type = (word lsr 8) land 0x7 in
  let command_retry_delay = (word lsr 11) land 0x3 in
  let more = (word lsr 13) land 1 = 1 in
  let do_not_retry = (word lsr 14) land 1 = 1 in
  let status =
    match status_code_type, status_code with
    | 0, 0x00 -> Success
    | 0, code -> Generic (generic_of_code code)
    | 1, code -> Command_specific (command_specific_of_code code)
    | 2, code -> Media (media_of_code code)
    | 3, code -> Path_related code
    | 7, code -> Vendor_specific code
    | sct, code -> Reserved (sct, code)
  in
  { status; status_code; status_code_type; do_not_retry; more; command_retry_delay }

let generic_to_string = function
  | Invalid_opcode -> "Invalid command opcode"
  | Invalid_field -> "Invalid field in command"
  | Command_id_conflict -> "Command identifier conflict"
  | Data_transfer_error -> "Data transfer error"
  | Aborted_power_loss -> "Aborted due to power loss notification"
  | Internal_error -> "Internal controller error"
  | Command_abort_requested -> "Command abort requested"
  | Abort_sq_deletion -> "Aborted due to submission queue deletion"
  | Fused_command_failed -> "Fused command failed"
  | Fused_command_missing -> "Fused command missing"
  | Invalid_namespace_or_format -> "Invalid namespace or format"
  | Command_sequence_error -> "Command sequence error"
  | Lba_out_of_range -> "LBA out of range"
  | Capacity_exceeded -> "Capacity exceeded"
  | Namespace_not_ready -> "Namespace not ready"
  | Unknown_generic code -> Printf.sprintf "Generic status 0x%02x" code

let command_specific_to_string = function
  | Completion_queue_invalid -> "Completion queue invalid"
  | Invalid_queue_identifier -> "Invalid queue identifier"
  | Invalid_queue_size -> "Invalid queue size"
  | Abort_command_limit_exceeded -> "Abort command limit exceeded"
  | Invalid_queue_deletion -> "Invalid queue deletion"
  | Feature_not_changeable -> "Feature identifier not changeable"
  | Feature_not_namespace_specific -> "Feature not namespace specific"
  | Namespace_identifier_unavailable -> "Namespace identifier unavailable"
  | Namespace_already_attached -> "Namespace already attached"
  | Namespace_not_attached -> "Namespace not attached"
  | Unknown_command_specific code -> Printf.sprintf "Command specific status 0x%02x" code

let media_to_string = function
  | Write_fault -> "Write fault"
  | Unrecovered_read_error -> "Unrecovered read error"
  | Guard_check_error -> "End to end guard check error"
  | Reference_tag_check_error -> "End to end reference tag check error"
  | Compare_failure -> "Compare failure"
  | Access_denied -> "Access denied"
  | Unknown_media code -> Printf.sprintf "Media status 0x%02x" code

let to_string = function
  | Success -> "Success"
  | Generic code -> generic_to_string code
  | Command_specific code -> command_specific_to_string code
  | Media code -> media_to_string code
  | Path_related code -> Printf.sprintf "Path related status 0x%02x" code
  | Vendor_specific code -> Printf.sprintf "Vendor specific status 0x%02x" code
  | Reserved (sct, code) -> Printf.sprintf "Reserved status type %u code 0x%02x" sct code

let is_success = function Success -> true | _ -> false

let describe decoded =
  if is_success decoded.status then "Success"
  else
    Printf.sprintf "%s (SCT=%u SC=0x%02x%s)" (to_string decoded.status)
      decoded.status_code_type decoded.status_code
      (if decoded.do_not_retry then " DNR" else "")
