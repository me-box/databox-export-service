open Lwt.Infix;

let create_content_format = (id) => {
  let bits = [%bitstring {|id : 16 : bigendian|}];
  Bitstring.string_of_bitstring(bits);
};

let req_endpoint = ref("tcp://127.0.0.1:5555");

let curve_server_key = ref("");

let curve_public_key = ref("");

let curve_secret_key = ref("");

let router_public_key = ref("");

let router_secret_key = ref("");

let token = ref("");

let uri_path = ref("");

let content_format = ref(create_content_format(50));

let identity = ref(Unix.gethostname());

let loop_count = ref(0);

let call_freq = ref(1.0);

let version = 1;

let test_result = ref("");

module Response = {
  type t =
    | OK
    | Unavailable
    | Payload(string)
    | Observe(string, string)
    | Error(string);
};

let setup_logger = () => {
  Lwt_log_core.default :=
    Lwt_log.channel(
    ~template="$(date).$(milliseconds) [$(level)] $(message)",
    ~close_mode=`Keep,
    ~channel=Lwt_io.stdout,
    ()
  );
  Lwt_log_core.add_rule("*", Lwt_log_core.Debug);
};

let to_hex = (msg) => Hex.(String.trim(of_string(msg) |> hexdump_s(~print_chars=false)));

let handle_header = (bits) => {
  let tuple = [%bitstring
    switch bits {
    | {|code : 8 : unsigned;
        oc : 8 : unsigned;
        tkl : 16 : bigendian;
        rest : -1 : bitstring
     |} => (
        tkl,
        oc,
        code,
        rest
      )
    | {|_|} => failwith("invalid header")
    }
  ];
  tuple;
};

let handle_option = (bits) => {
  let tuple = [%bitstring
    switch bits {
    | {|number : 16 : bigendian;
        len : 16 : bigendian;
        value: len*8: string;
        rest : -1 : bitstring
      |} => (
        number,
        value,
        rest
      )
    | {|_|} => failwith("invalid options")
    }
  ];
  tuple;
};

let handle_options = (oc, bits) => {
  let options = Array.make(oc, (0, ""));
  let rec handle = (oc, bits) =>
    if (oc == 0) {
      bits;
    } else {
      let (number, value, r) = handle_option(bits);
      options[oc - 1] = (number, value);
      let _ = Lwt_log_core.debug_f("option => %d:%s", number, value);
      handle(oc - 1, r);
    };
  (options, handle(oc, bits));
};

let has_public_key = (options) =>
  if (Array.exists(((number, _)) => number == 2048, options)) {
    true;
  } else {
    false;
  };

let get_option_value = (options, value) => {
  let rec find = (a, x, i) => {
    let (number, value) = a[i];
    if (number == x) {
      value;
    } else {
      find(a, x, i + 1);
    };
  };
  find(options, value, 0);
};

let handle_ack_content = (options, payload) => {
  let payload = Bitstring.string_of_bitstring(payload);
  if (has_public_key(options)) {
    let key = get_option_value(options, 2048);
    Response.Observe(key, payload) |> Lwt.return;
  } else {
    Response.Payload(payload) |> Lwt.return;
  };
};

let handle_ack_created = (options) => Response.OK |> Lwt.return;

let handle_ack_deleted = (options) => Response.OK |> Lwt.return;

let handle_service_unavailable = (options) => Response.Unavailable |> Lwt.return;

let handle_ack_bad_request = (options) => Response.Error("Bad Request") |> Lwt.return;

let handle_unsupported_content_format = (options) =>
  Response.Error("Unsupported Content-Format") |> Lwt.return;

let handle_ack_unauthorized = (options) => Response.Error("Unauthorized") |> Lwt.return;

let handle_not_acceptable = (options) => Response.Error("Not Acceptable") |> Lwt.return;

let handle_request_entity_too_large = (options) =>
  Response.Error("Request Entity Too Large") |> Lwt.return;

let handle_internal_server_error = (options) =>
  Response.Error("Internal Server Error") |> Lwt.return;

let handle_response = (msg) =>
  Lwt_log_core.debug_f("Received:\n%s", to_hex(msg))
  >>= (
    () => {
      let r0 = Bitstring.bitstring_of_string(msg);
      let (tkl, oc, code, r1) = handle_header(r0);
      let (options, payload) = handle_options(oc, r1);
      switch code {
      | 69 => handle_ack_content(options, payload)
      | 65 => handle_ack_created(options)
      | 66 => handle_ack_deleted(options)
      | 128 => handle_ack_bad_request(options)
      | 129 => handle_ack_unauthorized(options)
      | 143 => handle_unsupported_content_format(options)
      | 163 => handle_service_unavailable(options)
      | 134 => handle_not_acceptable(options)
      | 141 => handle_request_entity_too_large(options)
      | 160 => handle_internal_server_error(options)
      | _ => failwith("invalid code:" ++ string_of_int(code))
      };
    }
  );

let send_request = (~msg, ~_to as socket) =>
  Lwt_log_core.debug_f("Sending:\n%s", to_hex(msg))
  >>= (
    () =>
      Lwt_zmq.Socket.send(socket, msg) >>= (() => Lwt_zmq.Socket.recv(socket) >>= handle_response)
  );

let create_header = (~tkl, ~oc, ~code) => {
  let bits = [%bitstring
    {|code : 8 : unsigned;
      oc : 8 : unsigned;
      tkl : 16 : bigendian
    |}
  ];
  (bits, 32);
};

let create_option = (~number, ~value) => {
  let byte_length = String.length(value);
  let bit_length = byte_length * 8;
  let bits = [%bitstring
    {|number : 16 : bigendian;
      byte_length : 16 : bigendian;
      value : bit_length : string
    |}
  ];
  (bits, bit_length + 32);
};

let create_token = (~tk as token) => {
  let bit_length = String.length(token) * 8;
  (token, bit_length);
};

let create_options = (options) => {
  let count = Array.length(options);
  let values = Array.map(((x, y)) => x, options);
  let value = Bitstring.concat(Array.to_list(values));
  let lengths = Array.map(((x, y)) => y, options);
  let length = Array.fold_left((x, y) => x + y, 0, lengths);
  (value, length, count);
};

let create_get_options = (~uri, ~format) => {
  let uri_path = create_option(~number=11, ~value=uri);
  let uri_host = create_option(~number=3, ~value=identity^);
  let content_format = create_option(~number=12, ~value=format);
  create_options([|uri_path, uri_host, content_format|]);
};

let get = (~token=token^, ~format=content_format^, ~uri, ()) => {
  let (options_value, options_length, options_count) =
    create_get_options(~uri=uri, ~format=format);
  let (header_value, header_length) =
    create_header(~tkl=String.length(token), ~oc=options_count, ~code=1);
  let (token_value, token_length) = create_token(~tk=token);
  let bits = [%bitstring
    {|header_value : header_length : bitstring;
      token_value : token_length : string;
      options_value : options_length : bitstring
    |}
  ];
  Bitstring.string_of_bitstring(bits);
};

let set_main_socket_security = (soc) => {
  ZMQ.Socket.set_curve_serverkey(soc, curve_server_key^);
  ZMQ.Socket.set_curve_publickey(soc, curve_public_key^);
  ZMQ.Socket.set_curve_secretkey(soc, curve_secret_key^);
};

let connect_request_socket = (endpoint, ctx, kind) => {
  let soc = ZMQ.Socket.create(ctx, kind);
  set_main_socket_security(soc);
  ZMQ.Socket.connect(soc, endpoint);
  Lwt_zmq.Socket.of_socket(soc);
};

let close_socket = (lwt_soc) => {
  let soc = Lwt_zmq.Socket.to_socket(lwt_soc);
  ZMQ.Socket.close(soc);
};

let get_loop = (socket, count) => {
  let result = ref("");
  let rec loop = (n) =>
    send_request(~msg=get(~uri=uri_path^, ()), ~_to=socket)
    >>= (
      (resp) =>
        switch resp {
        | Response.OK =>
          if (n > 1) {
            Lwt_unix.sleep(call_freq^) >>= (() => loop(n - 1));
          } else {
            Lwt_io.printf("=> OK\n");
          }
        | Response.Payload(msg) =>
          if (n > 1) {
            Lwt_unix.sleep(call_freq^) >>= (() => loop(n - 1));
          } else
            {
              result := msg;
              Lwt.return_unit;
            } /* Lwt_io.printf "%s\n" msg; */
        | Response.Error(msg) => Lwt_io.printf("=> %s\n", msg)
        | Response.Unavailable => Lwt_io.printf("=> server unavailable\n")
        | _ => failwith("unhandled response")
        }
    );
  loop(count) >>= (() => Lwt.return(result^));
};

let get_test = (ctx) => {
  let req_soc = connect_request_socket(req_endpoint^, ctx, ZMQ.Socket.req);
  get_loop(req_soc, loop_count^)
  >>= (
    (res) => {
      test_result := res;
      close_socket(req_soc) |> Lwt.return;
    }
  );
} /* secret_key: j#3yqGG17QNTe(g@jJt6[LOg%ivqr<:}L%&NAUPt */ /* public_key: MP9pZzG25M2$.a%[DwU$OQ#-:C}Aq)3w*<AY^%V{ */; /* server_key: qDq63cJF5gd3Jed:/3t[F8u(ETeep(qk+%pmj(s? */

let setup_curve_keys = () => {
  let (public_key, private_key) = ZMQ.Curve.keypair();
  curve_public_key := public_key;
  curve_secret_key := private_key;
};

let setup_router_keys = () => {
  let (public_key, private_key) = ZMQ.Curve.keypair();
  router_public_key := public_key;
  router_secret_key := private_key;
};

type options = {
  request_endpoint: string,
  uri_path: string,
  identity: string,
  server_key: string,
  arbiter_token: string
};

let set_parameters = (opts) => {
  req_endpoint := opts.request_endpoint;
  uri_path := opts.uri_path;
  identity := opts.identity;
  curve_server_key := opts.server_key;
  token := opts.arbiter_token;
};

let get_secret = (opts, ~v as verbose=false, ()) => {
  let ctx = ZMQ.Context.create();
  setup_curve_keys();
  setup_router_keys();
  set_parameters(opts) /* log to screen debug info */ /* !file ? set_payload_from !payload : (); */ /* can take payload from a file */; /* parse_cmdline (); */
  verbose ? setup_logger() : (); /* Lwt_main.run {ctx |> !command}; */
  get_test(ctx)
  >>= (
    () => {
      ZMQ.Context.terminate(ctx);
      Lwt.return(test_result^);
    }
  );
}; /* let _ = try (Lwt_main.run {Lwt.return (client ())}) {
  | Invalid_argument msg => Printf.printf "Error: file not found!\n";
  | e => report_error e;
}; */