# mosquitto

MQTT broker for the mqttchan pipeline: `services/api` publishes to
`avatar/say`, the ESP32 in `firmware/` subscribes. Anonymous auth, no TLS —
acceptable only because this is reachable exclusively over LAN/Tailscale,
never the public internet.

**Moved here 2026-09-17** from `~/Homelab/osmo/mosquitto`, unchanged, when the
firmware, broker and services were collapsed into this monorepo. The Homelab
copy is now a stub pointing here.

## Deploy

This directory is a reference copy — osmo's runtime config is authoritative.
Deploy by copying it there and bringing it up with `docker compose`, not raw
`docker`, matching the rest of this homelab:

```
scp -r broker kevin@osmo:~/mosquitto
ssh kevin@osmo 'cd ~/mosquitto && docker compose up -d'
```

Verified 2026-09-17: the running container on osmo is byte-identical to the
files here, so this copy is current rather than drifted.

## Ports

- **1883** (raw MQTT/TCP) — what the ESP32 and `services/api` actually use.
  Connect via osmo's LAN (`10.0.0.5`) or Tailscale IP directly; not fronted
  by Caddy since it isn't HTTP.
- **9001** (MQTT-over-WebSockets) — for browser tools/future dashboards.
  Add this to osmo's live Caddyfile (not tracked here) to get a TLS +
  hostname endpoint:

  ```
  mqtt.kev.nz {
      reverse_proxy localhost:9001
  }
  ```

  The control page (`docs/index.html`) is the one thing that needs 9001: it
  runs in a browser, which cannot open a raw MQTT socket.

  Only 9001 goes through Caddy. Devices that speak plain MQTT should use
  1883 directly — no reason to pay for WebSocket framing on a
  memory-constrained device like the ESP32.

## Security note

Anonymous auth on both listeners. Fine under LAN/Tailscale-only access;
would need a password file (`mosquitto_passwd`) and `allow_anonymous false`
before this could ever be exposed further.
