Test-only TLS for the Peers relay tests (tests/peers_python_test.cpp, tests/peers_python):
`relay.pem` and `relay.key` are a certificate for 127.0.0.1 and localhost, signed by
`relay_ca.pem`, a CA made for these tests whose own key was thrown away. Nothing outside the
tests trusts it. Made with openssl, valid for 100 years.

`relay.pem` is the full chain, the certificate then the CA, as a server sends it: Windows'
Schannel finds a certificate's issuer only among what the server sent and the system's store,
not among the CAs the test adds.
