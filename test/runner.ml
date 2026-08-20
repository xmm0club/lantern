open Lantern

external run : string -> string -> int -> int = "lantern_selftest_entry"

let () =
  let argument index fallback =
    if Array.length Sys.argv > index then Sys.argv.(index) else fallback
  in
  let bdf = argument 1 Defaults.bdf in
  let image = argument 2 Defaults.selftest_image in
  let backend =
    match argument 3 "mock" with
    | "vfio" -> Ffi.backend_vfio
    | _ -> Ffi.backend_mock
  in
  exit (run bdf image backend)
