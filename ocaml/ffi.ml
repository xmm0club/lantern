type device

type config = {
  backend : int;
  admin_queue_depth : int;
  enable_msix : bool;
  mock_image_path : string;
  mock_capacity_bytes : int64;
  mock_lba_bytes : int;
  mock_namespaces : int;
  mock_latency_us : int;
  mock_serial : string;
  mock_model : string;
}

type dev_state = {
  cap : int64;
  vs : int32;
  cc : int32;
  csts : int32;
  aqa : int32;
  asq : int64;
  acq : int64;
  max_queue_entries : int;
  doorbell_stride : int;
  timeout_ms : int;
  page_size : int;
  max_transfer_bytes : int;
}

type queue_state = {
  active : bool;
  qid : int;
  depth : int;
  sq_tail : int;
  cq_head : int;
  cq_phase : int;
  sq_iova : int64;
  cq_iova : int64;
  submitted : int64;
  completed : int64;
}

type cqe = {
  dw0 : int32;
  sq_head : int;
  sq_id : int;
  cid : int;
  status : int;
}

type poll_result =
  | Idle
  | Completed of cqe
  | Failed of int

let ok = 0
let err_invalid = -1
let err_backend = -2
let err_no_device = -3
let err_timeout = -4
let err_nomem = -5
let err_controller = -6
let err_queue_full = -7
let err_state = -8
let err_range = -9

type error_code =
  | Invalid
  | Backend
  | No_device
  | Timeout
  | No_memory
  | Controller_fault
  | Queue_full
  | State
  | Range
  | Unknown of int

let decode_error_code code =
  if code = err_invalid then Invalid
  else if code = err_backend then Backend
  else if code = err_no_device then No_device
  else if code = err_timeout then Timeout
  else if code = err_nomem then No_memory
  else if code = err_controller then Controller_fault
  else if code = err_queue_full then Queue_full
  else if code = err_state then State
  else if code = err_range then Range
  else Unknown code

let error_code_name = function
  | Invalid -> "invalid"
  | Backend -> "backend"
  | No_device -> "no_device"
  | Timeout -> "timeout"
  | No_memory -> "no_memory"
  | Controller_fault -> "controller"
  | Queue_full -> "queue_full"
  | State -> "state"
  | Range -> "range"
  | Unknown code -> Printf.sprintf "unknown(%d)" code

let backend_vfio = 0
let backend_mock = 1

let sqe_bytes = 64
let cqe_bytes = 16
let page_size = 4096

let default_config = {
  backend = backend_mock;
  admin_queue_depth = 64;
  enable_msix = false;
  mock_image_path = "mock.img";
  mock_capacity_bytes = Int64.of_int (64 * 1024 * 1024);
  mock_lba_bytes = 512;
  mock_namespaces = 4;
  mock_latency_us = 0;
  mock_serial = "DEADBEEF";
  mock_model = "lantern mock nvme";
}

external dev_open : string -> config -> (device, string) result = "lantern_dev_open"
external dev_close : device -> int = "lantern_dev_close"
external dev_reset : device -> int = "lantern_dev_reset"
external dev_error : device -> string = "lantern_dev_error"
external dev_backend : device -> string = "lantern_dev_backend"
external dev_bdf : device -> string = "lantern_dev_bdf"
external strerror : int -> string = "lantern_strerror"
external dev_state : device -> dev_state = "lantern_dev_state"
external queue_state : device -> int -> queue_state = "lantern_queue_state"

external dma_alloc : device -> int -> (int, string) result = "lantern_dma_alloc"
external dma_free : device -> int -> unit = "lantern_dma_free"
external dma_iova : device -> int -> int64 = "lantern_dma_iova"
external dma_size : device -> int -> int = "lantern_dma_size"

external dma_write : device -> int -> int -> Bytes.t -> int -> int -> int
  = "lantern_dma_write_bytecode" "lantern_dma_write"

external dma_read : device -> int -> int -> Bytes.t -> int -> int -> int
  = "lantern_dma_read_bytecode" "lantern_dma_read"

external dma_fill : device -> int -> int -> int -> int -> int = "lantern_dma_fill"
external dma_pattern : device -> int -> int -> int -> int64 -> int = "lantern_dma_pattern"

external prp_build : device -> int -> int -> int -> int -> int -> Bytes.t -> int
  = "lantern_prp_build_bytecode" "lantern_prp_build"

external admin_submit : device -> Bytes.t -> int = "lantern_admin_submit"
external admin_poll : device -> int -> poll_result = "lantern_admin_poll"
external io_submit : device -> int -> Bytes.t -> int = "lantern_io_submit"
external io_poll : device -> int -> int -> poll_result = "lantern_io_poll"

external io_queue_alloc : device -> int -> int -> int = "lantern_io_queue_alloc"
external io_queue_addresses : device -> int -> (int64 * int64, string) result
  = "lantern_io_queue_addresses"
external io_queue_activate : device -> int -> int = "lantern_io_queue_activate"
external io_queue_free : device -> int -> int = "lantern_io_queue_free"

external irq_enable : device -> int -> int = "lantern_irq_enable"
external irq_disable : device -> int = "lantern_irq_disable"
external irq_raw_fd : device -> int -> int = "lantern_irq_fd"
external irq_count : device -> int = "lantern_irq_count"
external irq_wait : device -> int -> int -> int = "lantern_irq_wait"

external file_descr_of_int : int -> Unix.file_descr = "%identity"

let irq_file_descr device vector =
  let fd = irq_raw_fd device vector in
  if fd < 0 then None else Some (file_descr_of_int fd)
