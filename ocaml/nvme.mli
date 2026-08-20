type error =
  | Driver of { code : int; message : string }
  | Controller of { status : Status.decoded; command_id : int }
  | Timeout of string
  | Bad_argument of string

val string_of_error : error -> string

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

val default_config : config

type t
type queue
type request

val open_device : ?config:config -> string -> (t, error) result
val close : t -> unit
val reset : t -> (unit, error) result

val bdf : t -> string
val backend_name : t -> string
val interrupts_enabled : t -> bool
val registers : t -> Ffi.dev_state
val queue_registers : t -> int -> Ffi.queue_state

val identify_controller : t -> (Id.controller, error) result
val identify_namespace : t -> nsid:int -> (Id.namespace, error) result
val active_namespaces : t -> (int list, error) result
val smart_log : t -> (Id.smart, error) result
val block_size_of : t -> nsid:int -> (int, error) result
val set_number_of_queues : t -> submission:int -> completion:int -> (int * int, error) result

val create_io_queue : ?depth:int -> ?slot_bytes:int -> t -> (queue, error) result
val delete_io_queue : t -> queue -> (unit, error) result
val admin_queue : t -> queue
val io_queues : t -> queue list

val queue_id : queue -> int
val queue_depth : queue -> int
val queue_vector : queue -> int
val queue_outstanding : queue -> int
val queue_slot_bytes : queue -> int

val read_blocks :
  ?timeout_ms:int -> t -> queue -> nsid:int -> lba:int64 -> blocks:int -> (Bytes.t, error) result

val write_blocks :
  ?timeout_ms:int -> t -> queue -> nsid:int -> lba:int64 -> data:Bytes.t -> (unit, error) result

val write_zeroes :
  ?timeout_ms:int -> t -> queue -> nsid:int -> lba:int64 -> blocks:int -> (unit, error) result

val flush : ?timeout_ms:int -> t -> queue -> nsid:int -> (unit, error) result

val create_namespace : t -> blocks:int64 -> lba_format:int -> (int, error) result
val delete_namespace : t -> nsid:int -> (unit, error) result
val attach_namespace : t -> nsid:int -> (unit, error) result
val detach_namespace : t -> nsid:int -> (unit, error) result

val request_command_id : request -> int
val request_status : request -> Status.decoded option
val request_result_dword : request -> int32
val request_latency : request -> float

module Pipeline : sig
  type slot

  val capacity : queue -> int
  val outstanding : queue -> int
  val slot_request : slot -> request
  val slot_command_id : slot -> int
  val slot_length : slot -> int

  val submit_read :
    t -> queue -> nsid:int -> lba:int64 -> blocks:int -> block_bytes:int -> (slot, error) result

  val submit_write :
    ?payload:Bytes.t ->
    t -> queue -> nsid:int -> lba:int64 -> blocks:int -> block_bytes:int -> (slot, error) result

  val reap : t -> queue -> int -> ((request * float) option, error) result
  val read_slot : t -> queue -> slot -> (Bytes.t, error) result
end
