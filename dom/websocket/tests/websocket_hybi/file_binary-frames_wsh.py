from mod_pywebsocket import common, stream


def web_socket_do_extra_handshake(request):
    pass


def web_socket_transfer_data(request):
    messages_to_send = ["Hello, world!", "", all_distinct_bytes()]
    for message in messages_to_send:
        message = message.encode("latin-1")
        # FIXME: Should use better API to send binary messages when pywebsocket supports it.
        header = stream.create_header(common.OPCODE_BINARY, len(message), 1, 0, 0, 0, 0)
        request.connection.write(header + message)


def all_distinct_bytes():
    return "".join([chr(i) for i in range(256)])
