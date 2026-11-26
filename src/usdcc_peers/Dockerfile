# The relay of docs/PEERS.md §5.4, or the hub of §9, in a container.
#
#   docker build -t usdcc-peers src/usdcc_peers
#   docker run -d -p 8443:8443 -v /srv/relay:/data usdcc-peers \
#       usdcc-relay --port 8443 --token-file /data/relay.token
#   docker run -d -p 47800:47800 -p 8080:8080 -v /srv/hub:/data usdcc-peers \
#       usdcc-hub --port 47800 --http 8080 --token-file /data/hub.token --data /data --link-address hub.studio.example
#
# Put an HTTPS reverse proxy in front for wss://, or give the relay --certfile and --keyfile.
FROM python:3.13-slim

WORKDIR /opt/usdcc_peers
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY pyproject.toml README.md ./
COPY usdcc_peers ./usdcc_peers
RUN pip install --no-cache-dir --no-deps . && useradd --system --home /data peers && mkdir -p /data && chown peers /data
USER peers
VOLUME /data
EXPOSE 8443
CMD ["usdcc-relay", "--port", "8443"]
