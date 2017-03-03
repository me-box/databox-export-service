
open Lwt
open Opium.Std

module Client = Cohttp_lwt_unix.Client

type request = {
    id   : Uuidm.t;
    url  : Uri.t;
    body : Cohttp_lwt_body.t; }

type response = {
    status : Cohttp.Code.status_code;
    body   : Cohttp_lwt_body.t; }


type state = [`Pending | `Processing | `Finished of response]


type queue = {
    t    : request Lwt_stream.t;
    stbl : (Uuidm.t, state) Hashtbl.t;
    push : request option -> unit; }


let string_of_state = function
  | `Pending -> "Pending"
  | `Processing -> "Processing"
  | `Finished _ -> "Finished"


let export q =
  let handler q req =
    let body = Request.body req in
    Cohttp_lwt_body.to_string body >>= fun body ->
    Logs_lwt.info (fun m -> m "body: %s\n" body) >>= fun () ->

    let open Ezjsonm in
    let obj =
      from_string body
      |> value
      |> get_dict
    in
    let id =
      List.assoc "id" obj
      |> get_string
    in

    if id = "" then
      let url =
        List.assoc "dest" obj
        |> get_string
      in
      let body =
        List.assoc "data" obj
        |> get_dict
        |> fun d -> to_string (`O d)
      in
      let id = Uuidm.create `V4 in
      let request = {
          id;
          url = Uri.of_string url;
          body = Cohttp_lwt_body.of_string body}
      in
      let state = `Pending in
      let r = dict [
          "id", Uuidm.to_string id |> string;
          "state", string_of_state state |> string;
          "response", "" |> string;]
      in
      let () = Hashtbl.add q.stbl id state in
      let () = q.push (Some request) in
      `Json r |> respond'
    else
      let id = match Uuidm.of_string id with
        | None -> raise @@ Invalid_argument id
        | Some uuid -> uuid
      in
      match Hashtbl.find q.stbl id with
      | `Finished {status; body} as s -> begin
         Cohttp_lwt_body.to_string body >>= fun body ->
         let r = dict [
           "status", Cohttp.Code.string_of_status status |> string;
           "body", body |> string; ]
           |> value
         in
         let resp = dict [
           "id", Uuidm.to_string id |> string;
           "state", string_of_state s |> string;
           "response", r;]
         in
         let () = Hashtbl.remove q.stbl id in
         `Json resp |> respond' end
      | #state as state ->
         let resp = dict [
           "id", Uuidm.to_string id |> string;
           "state", string_of_state state |> string;
           "response", "" |> string;]
         in
         `Json resp |> respond'
  in
  post "/export" @@ handler q


let worker_t q =
  let process {id; url; body} =
    let () = Hashtbl.replace q.stbl id `Processing in
    Client.post ~body url >>= fun (resp, body) ->
    let status = Cohttp.Response.status resp in
    let resp = {status; body} in
    let state = `Finished resp in
    let () = Hashtbl.replace q.stbl id state in
    return_unit
  in
  let rec aux () =
    Lwt_stream.get q.t >>= function
    | None -> Logs_lwt.warn (fun m -> m "Got None...(stream closed?)")
    | Some r ->
        process r >>= fun () ->
        Logs_lwt.info (fun m -> m "Finished processing of a request!") >>=
        aux
  in
  aux ()


let base_app () =
  let p = Export_env.local_port () |> int_of_string in
  let () = Logs.info (fun m -> m "serving on port %d" p) in

  let env_cert = Export_env.cert_path ()
  and env_key  = Export_env.key_path () in

  let app = App.empty |> App.port p in

  if env_cert = "" || env_key = "" then app else
  let open Bos.OS in
  let open Rresult.R.Infix in
  (Dir.user () >>= fun user ->
  let cert_dir =
    Fpath.add_seg user "certs"
    |> Fpath.to_dir_path
  in
  Dir.create cert_dir >>= fun _ ->

  let file_cert = Fpath.add_seg cert_dir "public.cert"
  and file_key  = Fpath.add_seg cert_dir "private.key" in
  File.delete file_cert >>= fun () ->
  File.delete file_key  >>= fun () ->
  File.write file_cert env_cert >>= fun () ->
  File.write file_key env_key   >>= fun () ->
  let cert = Fpath.to_string file_cert in
  let key  = Fpath.to_string file_key in
  Ok (cert, key))
  |> function
  | Error msg ->
      Logs.err(fun m ->
          m "while installing https certs: %a" Rresult.R.pp_msg msg);
      app
  | Ok (cert, key) ->
      app |> App.ssl ~cert ~key


let t () =
  let t, push = Lwt_stream.create () in
  let stbl = Hashtbl.create 13 in
  let queue = {t; stbl; push} in

  let app =
    base_app ()
    |> middleware Macaroon.macaroon_verifier_mw
    |> export queue in

  let export_queue =
    match App.run_command' app with
    | `Ok t -> t
    | _ -> assert false
  in

  Lwt.join [
    export_queue;
    worker_t queue; ]
