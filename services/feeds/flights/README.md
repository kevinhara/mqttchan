# feeds/flights

Announces arrivals and departures at one airport, from live ADS-B. Configured
for **ZQN** (Queenstown, NZQN); the airport is three environment variables, so
pointing it elsewhere needs no code change.

```
node feeds/flights/dist/index.js --once --dry-run
```

## How a movement is detected

There is no arrivals/departures API in play — only where every aircraft is
right now — so a movement is an **edge**: the same airframe seen on the ground
at the field after being airborne, or the reverse. `src/movements.ts` is the
whole of it, and is pure so it can be tested without the network.

Four rules stop that edge firing when nothing happened. Each one is a real
false positive that was observed, not a hypothetical:

- **Ignore non-transponder contacts (`type` ending `_nt`).** NZQN's 30nm circle
  holds six of them, two self-identifying as `TWR`. They are ADS-B ground
  infrastructure, they sit permanently at `alt_baro: "ground"`, and without
  this they look exactly like aircraft parked at the airport.
- **A first sighting is silent.** It records where the aircraft is and says
  nothing, because there is no way to tell a fresh arrival from something that
  has been on the apron for an hour. Without this, every start of the feed
  announces the whole apron as arriving at once.
- **"On the ground" means within 1.5nm of the reference point.** Cromwell is
  ~25nm away, inside the watch circle. An aircraft on the ground anywhere else
  is *ignored* rather than tracked, which deliberately leaves its remembered
  airborne state intact — so if it later comes here, the arrival still fires.
- **Ten-minute cooldown per airframe per movement.** A circuit or a go-around
  produces genuine ground/air edges that nobody wants told twice.

Forgetting an aircraft after 45 minutes unseen can only ever cost a missed
announcement, never invent one: a forgotten aircraft is re-baselined in
silence. That asymmetry is what makes the expiry safe.

The ground/air state comes from `alt_baro == "ground"`, which is the aircraft's
own air/ground status bit (weight-on-wheels), not a guess derived from
altitude — so no field-elevation threshold is needed or wanted.

## Notes on the source (adsb.lol)

Both of these cost an hour on 2026-09-18 and neither is documented anywhere
obvious:

- **A generic User-Agent is rejected** — `403 "User-Agent too generic; include
  valid contact info."` Node's default UA trips it; curl's does not, which is
  why a curl spike succeeds and the obvious `fetch()` port of it fails with
  what looks like an unrelated error.
- **`fetch()` cannot be used.** api.adsb.lol publishes AAAA records whose
  addresses blackhole from this network (`curl -6` never connects, `curl -4`
  returns 200), and `fetch()` hangs until `ETIMEDOUT` with no way to say "IPv4
  only" — `dns.setDefaultResultOrder("ipv4first")` does not help and undici's
  happy-eyeballs fallback does not rescue it. `node:https` takes `family: 4`
  natively, so `src/adsb.ts` uses it. Set `ADSB_IP_FAMILY=0` to hand the choice
  back to the OS somewhere with working IPv6 to them.

Chosen over a local SDR because that was already the recorded decision: the
predecessor service's README named "airplanes.live or adsb.lol keyed to
lat/lon + radius, not local SDR hardware". airplanes.live serves the same
`/v2/point` shape and is the fallback if this goes away.

## Verified

2026-09-18, against the live API and a real movement:

- A 30nm point query on NZQN returns real traffic, and `--once --dry-run`
  classifies it correctly: 6 contacts, 5 of them ground infrastructure marked
  `ignored`, ANZ611 (ZK-OYB, A21N) marked `ground` at 0.0nm.
- **A complete real arrival was captured and replayed through the detector.**
  15 snapshots at 31s intervals, ANZ611 descending 5550ft at 11.8nm through to
  `ground` at 0.27nm and taxiing in at 3kt. The detector announced
  `ANZ611 landed at ZQN` exactly once, on the first ground snapshot — and
  stayed silent for the 9 airborne snapshots before it and the 5 ground
  snapshots after.
- Against a locally running API: two distinct movements `queued`, and a repeat
  of the same airframe's same movement `deduped`. `kind: "notice"` is accepted
  by triage as-is.

**Departure confirmed live 2026-09-18**, 09:20 NZST: `ZKICH left ZQN`,
published from the deployed container within twenty minutes of the stack coming
up. `flights` recorded submitted 1 / queued 1 / published 1 — no duplicate from
the aircraft's subsequent climb snapshots, nothing dropped, queue drained. This
replaces an earlier note here saying a departure had only unit tests behind it;
it was accurate when written and is now superseded.

Still unobserved in production: the cooldown and the 45-minute expiry actually
firing, since neither has had a circuit or a coverage dropout to act on yet.
