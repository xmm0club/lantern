let admin_delete_sq = 0x00
let admin_create_sq = 0x01
let admin_get_log_page = 0x02
let admin_delete_cq = 0x04
let admin_create_cq = 0x05
let admin_identify = 0x06
let admin_abort = 0x08
let admin_set_features = 0x09
let admin_get_features = 0x0a
let admin_namespace_management = 0x0d
let admin_namespace_attachment = 0x15
let admin_format_nvm = 0x80

let io_flush = 0x00
let io_write = 0x01
let io_read = 0x02
let io_write_zeroes = 0x08

let cns_namespace = 0x00
let cns_controller = 0x01
let cns_namespace_list = 0x02

let feature_number_of_queues = 0x07

let log_page_smart = 0x02

let create () = Bytes.make Ffi.sqe_bytes '\000'

let set_dword sqe index value =
  Bytes.set_int32_le sqe (index * 4) value

let get_dword sqe index = Bytes.get_int32_le sqe (index * 4)

let set_cdw sqe index value = set_dword sqe index (Int32.of_int value)

let set_opcode_cid sqe ~opcode ~cid =
  set_dword sqe 0 (Int32.logor (Int32.of_int (opcode land 0xff))
                     (Int32.shift_left (Int32.of_int (cid land 0xffff)) 16))

let set_nsid sqe nsid = set_dword sqe 1 (Int32.of_int nsid)

let set_prp1 sqe iova =
  set_dword sqe 6 (Int64.to_int32 iova);
  set_dword sqe 7 (Int64.to_int32 (Int64.shift_right_logical iova 32))

let set_prp2 sqe iova =
  set_dword sqe 8 (Int64.to_int32 iova);
  set_dword sqe 9 (Int64.to_int32 (Int64.shift_right_logical iova 32))

let prp2 sqe =
  Int64.logor
    (Int64.logand (Int64.of_int32 (get_dword sqe 8)) 0xffffffffL)
    (Int64.shift_left (Int64.logand (Int64.of_int32 (get_dword sqe 9)) 0xffffffffL) 32)

let set_lba sqe lba =
  set_dword sqe 10 (Int64.to_int32 lba);
  set_dword sqe 11 (Int64.to_int32 (Int64.shift_right_logical lba 32))

let base ~opcode ~cid ~nsid =
  let sqe = create () in
  set_opcode_cid sqe ~opcode ~cid;
  set_nsid sqe nsid;
  sqe

let identify_controller ~cid ~iova =
  let sqe = base ~opcode:admin_identify ~cid ~nsid:0 in
  set_prp1 sqe iova;
  set_cdw sqe 10 cns_controller;
  sqe

let identify_namespace ~cid ~nsid ~iova =
  let sqe = base ~opcode:admin_identify ~cid ~nsid in
  set_prp1 sqe iova;
  set_cdw sqe 10 cns_namespace;
  sqe

let identify_namespace_list ~cid ~start ~iova =
  let sqe = base ~opcode:admin_identify ~cid ~nsid:start in
  set_prp1 sqe iova;
  set_cdw sqe 10 cns_namespace_list;
  sqe

let create_io_completion_queue ~cid ~qid ~depth ~iova ~vector ~interrupts =
  let sqe = base ~opcode:admin_create_cq ~cid ~nsid:0 in
  set_prp1 sqe iova;
  set_cdw sqe 10 (((depth - 1) lsl 16) lor qid);
  set_cdw sqe 11 ((vector lsl 16) lor (if interrupts then 0x2 else 0x0) lor 0x1);
  sqe

let create_io_submission_queue ~cid ~qid ~depth ~cqid ~iova =
  let sqe = base ~opcode:admin_create_sq ~cid ~nsid:0 in
  set_prp1 sqe iova;
  set_cdw sqe 10 (((depth - 1) lsl 16) lor qid);
  set_cdw sqe 11 ((cqid lsl 16) lor 0x1);
  sqe

let delete_io_submission_queue ~cid ~qid =
  let sqe = base ~opcode:admin_delete_sq ~cid ~nsid:0 in
  set_cdw sqe 10 qid;
  sqe

let delete_io_completion_queue ~cid ~qid =
  let sqe = base ~opcode:admin_delete_cq ~cid ~nsid:0 in
  set_cdw sqe 10 qid;
  sqe

let set_number_of_queues ~cid ~submission ~completion =
  let sqe = base ~opcode:admin_set_features ~cid ~nsid:0 in
  set_cdw sqe 10 feature_number_of_queues;
  set_cdw sqe 11 (((completion - 1) lsl 16) lor (submission - 1));
  sqe

let get_number_of_queues ~cid =
  let sqe = base ~opcode:admin_get_features ~cid ~nsid:0 in
  set_cdw sqe 10 feature_number_of_queues;
  sqe

let smart_log ~cid ~iova ~dwords =
  let sqe = base ~opcode:admin_get_log_page ~cid ~nsid:0xffffffff in
  set_prp1 sqe iova;
  set_cdw sqe 10 (((dwords - 1) lsl 16) lor log_page_smart);
  sqe

let abort ~cid ~target_sqid ~target_cid =
  let sqe = base ~opcode:admin_abort ~cid ~nsid:0 in
  set_cdw sqe 10 ((target_cid lsl 16) lor target_sqid);
  sqe

let namespace_create ~cid ~iova =
  let sqe = base ~opcode:admin_namespace_management ~cid ~nsid:0 in
  set_prp1 sqe iova;
  set_cdw sqe 10 0;
  sqe

let namespace_delete ~cid ~nsid =
  let sqe = base ~opcode:admin_namespace_management ~cid ~nsid in
  set_cdw sqe 10 1;
  sqe

let namespace_attach ~cid ~nsid ~iova =
  let sqe = base ~opcode:admin_namespace_attachment ~cid ~nsid in
  set_prp1 sqe iova;
  set_cdw sqe 10 0;
  sqe

let namespace_detach ~cid ~nsid ~iova =
  let sqe = base ~opcode:admin_namespace_attachment ~cid ~nsid in
  set_prp1 sqe iova;
  set_cdw sqe 10 1;
  sqe

let format ~cid ~nsid ~lba_format =
  let sqe = base ~opcode:admin_format_nvm ~cid ~nsid in
  set_cdw sqe 10 lba_format;
  sqe

let read ~cid ~nsid ~lba ~blocks =
  let sqe = base ~opcode:io_read ~cid ~nsid in
  set_lba sqe lba;
  set_cdw sqe 12 (blocks - 1);
  sqe

let write ~cid ~nsid ~lba ~blocks =
  let sqe = base ~opcode:io_write ~cid ~nsid in
  set_lba sqe lba;
  set_cdw sqe 12 (blocks - 1);
  sqe

let write_zeroes ~cid ~nsid ~lba ~blocks =
  let sqe = base ~opcode:io_write_zeroes ~cid ~nsid in
  set_lba sqe lba;
  set_cdw sqe 12 (blocks - 1);
  sqe

let flush ~cid ~nsid = base ~opcode:io_flush ~cid ~nsid
