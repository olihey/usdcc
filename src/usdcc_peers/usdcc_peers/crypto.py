"""libsodium, as the protocol uses it (docs/PEERS_PROTOCOL.md §2, §3).

The same calls as the app's `src/cpp/peers/src/crypto.cpp`, through PyNaCl's bindings:
the handshake's X25519 keys (crypto_kx), XChaCha20-Poly1305 for a connection's own
messages and for the room's, Ed25519 for who signed what, and a crypto_box between two
members' keys for a private message.
"""

from __future__ import annotations

import base64
import os
import struct

from nacl import bindings as sodium
from nacl.exceptions import BadSignatureError, CryptoError
from nacl.signing import SigningKey, VerifyKey

SEALED_MARKER = 1
ROOM_MARKER = 2
NONCE_BYTES = sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES  # 24
TAG_BYTES = sodium.crypto_aead_xchacha20poly1305_ietf_ABYTES  # 16
SIGNATURE_BYTES = 64
_BASE32 = "abcdefghijklmnopqrstuvwxyz234567"


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def unb64(text) -> bytes:
    if not isinstance(text, str) or not text:
        return b""
    try:
        return base64.b64decode(text + "=" * (-len(text) % 4))
    except ValueError:
        return b""


def base32(data: bytes) -> str:
    """Lower-case base32 without padding, as the app writes fingerprints and ids."""
    result = []
    buffer = 0
    bits = 0
    for byte in data:
        buffer = ((buffer << 8) | byte) & 0xFFFFFFFF
        bits += 8
        while bits >= 5:
            result.append(_BASE32[(buffer >> (bits - 5)) & 31])
            bits -= 5
    if bits > 0:
        result.append(_BASE32[(buffer << (5 - bits)) & 31])
    return "".join(result)


def random_id(characters: int) -> str:
    return base32(os.urandom((characters * 5 + 7) // 8))[:characters]


def random_secret() -> str:
    return base64.urlsafe_b64encode(os.urandom(16)).decode("ascii").rstrip("=")


def fingerprint_of(public_key: bytes) -> str:
    """32 lower-case base32 characters of a 20-byte BLAKE2b hash of the key."""
    return base32(sodium.crypto_generichash_blake2b_salt_personal(public_key, digest_size=20))


def short_fingerprint(fingerprint: str) -> str:
    return fingerprint[:4] + "-" + fingerprint[4:8]


def same_secret(given, secret: str) -> bool:
    """Compared in constant time, so the time taken doesn't tell how much was right."""
    if not isinstance(given, str) or not secret:
        return False
    a = given.encode("utf-8")
    b = secret.encode("utf-8")
    if len(a) != len(b):
        return False
    difference = 0
    for x, y in zip(a, b):
        difference |= x ^ y
    return difference == 0


class Identity:
    """An Ed25519 key pair: who a peer is (PEERS.md §4).

    `secret_key` is libsodium's 64 bytes (seed then public key), the same bytes the app
    keeps in its identity file, so a key file can move between the two.
    """

    def __init__(self, signing_key: SigningKey):
        self._signing = signing_key

    @classmethod
    def generate(cls) -> Identity:
        return cls(SigningKey.generate())

    @classmethod
    def from_secret_key(cls, secret_key: bytes) -> Identity:
        if len(secret_key) == 64:
            identity = cls(SigningKey(secret_key[:32]))
            if identity.secret_key == secret_key:
                return identity
        raise ValueError("Not an Ed25519 secret key of 64 bytes")

    @classmethod
    def load_or_create(cls, path) -> Identity:
        """From a key file, made on first use and readable by its owner only."""
        path = os.fspath(path)
        try:
            with open(path, "rb") as file:
                return cls.from_secret_key(file.read())
        except FileNotFoundError:
            pass
        identity = cls.generate()
        folder = os.path.dirname(path)
        if folder:
            os.makedirs(folder, exist_ok=True)
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "wb") as file:
            file.write(identity.secret_key)
        return identity

    @property
    def public_key(self) -> bytes:
        return bytes(self._signing.verify_key)

    @property
    def secret_key(self) -> bytes:
        return bytes(self._signing) + self.public_key

    @property
    def fingerprint(self) -> str:
        return fingerprint_of(self.public_key)

    def sign(self, message: bytes) -> bytes:
        return self._signing.sign(message).signature


def verify(public_key: bytes, message: bytes, signature: bytes) -> bool:
    if len(public_key) != 32 or len(signature) != SIGNATURE_BYTES:
        return False
    try:
        VerifyKey(public_key).verify(message, signature)
        return True
    except (BadSignatureError, ValueError):
        return False


def kx_key_pair() -> tuple[bytes, bytes]:
    """(public, secret) X25519 keys for one connection."""
    return sodium.crypto_kx_keypair()


def kx_session_keys(as_host: bool, public: bytes, secret: bytes, theirs: bytes):
    """(receive, send) for this side of a connection, or None for a bad key."""
    if len(theirs) != 32:
        return None
    try:
        if as_host:
            return sodium.crypto_kx_server_session_keys(public, secret, theirs)
        return sodium.crypto_kx_client_session_keys(public, secret, theirs)
    except CryptoError:
        return None


class Sealer:
    """One direction of a connection: the nonce is a counter both sides keep."""

    def __init__(self, key: bytes = b""):
        self.key = key
        self.counter = 0

    def _nonce(self) -> bytes:
        return struct.pack("<Q", self.counter) + bytes(16)

    def seal(self, plain: bytes) -> bytes:
        cipher = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(plain, None, self._nonce(), self.key)
        self.counter += 1
        return bytes([SEALED_MARKER]) + cipher

    def open(self, envelope: bytes):
        if len(envelope) < 1 + TAG_BYTES or envelope[0] != SEALED_MARKER:
            return None
        try:
            plain = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(envelope[1:], None, self._nonce(), self.key)
        except CryptoError:
            return None
        self.counter += 1
        return plain


def seal_for_room(room_key: bytes, epoch: int, plain: bytes, host: Identity) -> bytes:
    """[2][epoch u32 BE][nonce][signature][ciphertext]; the host signs epoch, nonce and ciphertext."""
    nonce = os.urandom(NONCE_BYTES)
    cipher = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(plain, None, nonce, room_key)
    head = struct.pack(">I", epoch) + nonce
    return bytes([ROOM_MARKER]) + head + host.sign(head + cipher) + cipher


def room_epoch(envelope: bytes):
    if len(envelope) < 5 or envelope[0] != ROOM_MARKER:
        return None
    return struct.unpack(">I", envelope[1:5])[0]


def open_from_room(room_keys: dict, host_key: bytes, envelope: bytes):
    head = 1 + 4 + NONCE_BYTES + SIGNATURE_BYTES
    if len(envelope) < head + TAG_BYTES or envelope[0] != ROOM_MARKER:
        return None
    key = room_keys.get(struct.unpack(">I", envelope[1:5])[0])
    if not key:
        return None
    nonce = envelope[5:5 + NONCE_BYTES]
    signature = envelope[5 + NONCE_BYTES:head]
    cipher = envelope[head:]
    if not verify(host_key, envelope[1:5 + NONCE_BYTES] + cipher, signature):
        return None
    try:
        return sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(cipher, None, nonce, key)
    except CryptoError:
        return None


def _box_keys(mine: Identity, their_public_key: bytes):
    if len(their_public_key) != 32:
        return None
    try:
        return (sodium.crypto_sign_ed25519_pk_to_curve25519(their_public_key),
                sodium.crypto_sign_ed25519_sk_to_curve25519(mine.secret_key))
    except CryptoError:
        return None


def seal_for_peer(mine: Identity, their_public_key: bytes, plain: bytes) -> bytes:
    """[nonce 24][crypto_box_easy], which only the two of them can open (PEERS.md §6.2)."""
    keys = _box_keys(mine, their_public_key)
    if not keys:
        return b""
    nonce = os.urandom(sodium.crypto_box_NONCEBYTES)
    return nonce + sodium.crypto_box_easy(plain, nonce, keys[0], keys[1])


def open_from_peer(mine: Identity, their_public_key: bytes, sealed: bytes):
    keys = _box_keys(mine, their_public_key)
    if not keys or len(sealed) < sodium.crypto_box_NONCEBYTES + 16:
        return None
    try:
        return sodium.crypto_box_open_easy(sealed[sodium.crypto_box_NONCEBYTES:],
                                           sealed[:sodium.crypto_box_NONCEBYTES], keys[0], keys[1])
    except CryptoError:
        return None
