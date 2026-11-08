# 181: the room's region chosen by its creator, a smarter Auto, and a race ping beside the server's

Status: in progress

Branch `feat/sfu-region-ping` from `main` db3702d (0.7.13), started 2026-11-08. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Players, above all in South America, Africa and the Middle East, see a high PING in rooms they
create. That number is the round trip to the room's Durable Object, which runs in about 31
Cloudflare cities, placed once from the creator's country or continent (finding 135); `sam`, `afr`
and `me` host none, so those rooms run on another continent. Race frames never touch the room:
they go through each player's nearest Cloudflare Realtime location (findings 158, 177). So players
judge races by the wrong number.

Three parts, all chosen by the owner:

1. **A race ping beside the server's.** In the room and in races the FPS & Ping counter shows the
   room server's round trip and the delay races actually have, each named so it reads right, at
   most 13 characters a line (finding 169's Rust test). dhd.log keeps lines for both.
2. **The creator picks the room's region.** Create gets a choice: Auto (the default) or one of
   the Durable Object regions that run objects. It becomes the `locationHint`, shows in the list
   and the room header, and dhd.ini keeps the last choice. Protocol: additions only; 0.7.13 keeps
   working against the new server and `MIN_VERSION` stays 0.7.13 (docs/online-protocol.md).
3. **A smarter Auto.** Place by the Cloudflare location (colo) the creator reached, mapped to the
   nearest Durable Object region by a table built from Cloudflare's published locations (and
   measured round trips where possible), falling back to country and continent as now. Unit tests
   for the mapping; replay the last days' real rooms from the logs.

Acceptance:
- Unit tests: server (placement, the region choice, a 0.7.13 hello and create unchanged) and Rust/C
  for the counter text.
- `python tools/check.py port` and `python tools/check.py server` pass; boot and race state traces
  equal db3702d's.
- A temporary test Worker (deleted after): rooms created from this PC with each region and Auto,
  lobby and race pings recorded; a race with the PC and nettool bots, 6+ racers.

Out of scope: P2P rooms (another lane, branch `feat/p2p-clean`), moving a room after it is made.

## Progress

- [x] 2026-11-08: plan (this file), b868e84
- [x] 2026-11-08: server: the creator's pick (`?region=`), Auto by colo, `area` names, tests: 4755827;
      the table limited to what measurements say: 667bee6
- [x] 2026-11-08: client: Room Region row and `room_region` in dhd.ini, the area in the list and the
      room header, the counter's SERVER / RACE / RIDERS lines, dhd.log: 0f0febf
- [x] 2026-11-08: checks: `check.py server` OK, `check.py port` OK, boot (600) and race (5000) state
      traces equal 0.7.13's
- [x] 2026-11-08: test Worker: pings per region and Auto from this PC; a 7-racer race (this PC's game
      and 6 nettool bots); both test Workers deleted
- [x] 2026-11-08: log replay of 427 rooms made in the 7 days before

## Decisions

- **The race ping is the SFU association's ICE round trip**, not the snapshot age. str0m reports the
  selected candidate pair's latest STUN check (`CandidatePairStats::current_round_trip_time`, its
  statistics turned on at 1 s, `port/render/net/src/sfu.rs`). It exists in the room as well as in a
  race (the session is made at join, finding 177), it is a round trip from this PC like the server's
  ping, so the two read side by side, and it includes the TURN relay when that carries the race. The
  C client samples it once a second (`client_race_rtt`) and the counter shows the median of the last
  5, as the server ping does.
- **Labels: `SERVER N MS` and `RACE N MS`; in a race `RIDERS N MS` replaces `RACE`.** SERVER names
  what the old `N MS PING` measured (the room's server); RACE is where race data goes. In a race the
  riders' delay (finding 169) is the number that matters and stays; `RACE` comes back in a race until
  a rider is heard from. Each number stops at 999, so a line is at most 13 characters (`SERVER 999
  MS`); the bitmap went from 128x22 to 128x32 for the third line. In no room only `N FPS` shows.
- **dhd.log:** the 30 s `net:` line adds `race network N ms median, M worst (K samples)`; where the
  room runs names its area and `picked by its creator`.
- **Regions offered:** the seven hints that run objects of their own, named `EU WEST` (weur: AMS LHR
  CDG MAD MRS LIS), `EU EAST` (eeur: FRA MXP WAW VIE OTP ARN PRG), `NA EAST` (enam: ATL EWR MIA ORD
  IAD), `NA WEST` (wnam: DFW SJC LAX DEN SEA), `ASIA NE` (apac-ne: NRT KIX ICN), `ASIA SE` (apac-se:
  SIN HKG), `OCEANIA` (oc: SYD MEL BNE). Not offered: `sam`, `afr`, `me` (no objects; Cloudflare puts
  them in enam, weur and eeur, its own region pages show the same colos) and `apac` (spreads rooms over
  both Asian sub-regions). The menu labels come from the one settings table (`EU West`...); the
  server's area names are in capitals like the existing `EU`.
- **The wire:** `/v1/rooms/new?region=<hint>`, a query parameter because the room is made before the
  hello. Anything else (none, `auto`, `sam`, garbage) is Auto, so no game fails to make a room.
  `welcome` adds `roomArea` and `roomChosen`, `where` and the list add `area`. Additions only:
  `MIN_VERSION` stays; a 0.7.13 hello and create are tested (`test/room-region.test.ts`).
- **The menu:** a value row `Room Region: < Auto >` on ONLINE right below the two Create rows
  (Left/Right/Cross change it, saved to dhd.ini as `room_region`, `DHD_NET_ROOM_REGION`). Join With A
  Code and Update Now move down one; HOME's later rows got names (`HOME_REGION`...).
- **Auto by the colo the creator reached.** Their ping to the room is their way to that colo (the
  same wherever the room runs) plus the colo's way to the room, so the colo, not the country, decides.
  The table (`online/server/src/colo-hints.ts`) is generated by `online/server/tools/do_locations.mjs`
  from `tools/do-locations.json` (where.durableobjects.live: 341 colos, 31 run objects, each hint's
  colos) and `tools/do-rtt.json` (measured). A colo gets a row only when the data speaks for it: 62
  measured, 138 within 600 km of a measured one, 4 that run objects themselves (their own region).
  Every other colo falls back to the country and continent (finding 135). A hint costs the mean of its
  two quickest colos from that edge, because a hint's objects land in its colos near the edge (below).
  `npm test` runs `do_locations.mjs check` first.
- **Measured how:** a temporary Worker (`dhd-region-probe`, deleted) with one Durable Object placed
  in each of the 31 colos (placed by hints from this PC and from Globalping probes in NL, ES, PT, FR,
  BE, GB, KR until every colo had one; each object learns its colo from a trace). Its `/probe` times
  three calls to each object from the Worker at the edge the request reached, keeping the quicker of
  the last two. 157 Globalping HTTP probes (globalping.io, 4 rounds by country and city, about 280 of
  the free hourly tests) and this PC called it, reaching 62 edges. Per edge the quickest answer per
  colo is kept.

## Surprises

- **A hint doesn't spread its objects the way where.durableobjects.live's shares say.** Those are
  averages over all edges. From CDG, 52 weur objects all went to CDG or LHR; from MAD, 20 went to MAD,
  MRS and LIS; from LHR, to LHR and AMS. eeur and wnam from CDG did spread over all their colos. So
  the hint picks the region and Cloudflare picks colos near the edge in it.
- **Great circles are wrong for the far places.** The first table (distance, global shares) put
  Johannesburg, Beirut, Kuwait and Tel Aviv in eeur; measured, weur is 40, 18, 19 and 12 ms quicker.
  Fortaleza and Recife came out weur across the Atlantic, unmeasured; Muscat and Bahrain are a
  toss-up. Distance-only rows were dropped.
- **This PC's two paths.** From Algeria, Worker traffic enters at MAN, LHR or CDG (the ISP's routing:
  a room in the edge's own colo still pings 64-118 ms), but Cloudflare Realtime answers in 7-9 ms,
  from a colo in the country. That's the players' complaint in one PC: SERVER 150, RACE 8.
- **Where players reach Cloudflare** (427 rooms made 2026-11-01..08, Workers Logs `request.cf`):
  SIN/ID 58, GRU/BR 39, GIG/PE 36, EZE/PE 23, GRU/PE 18, MRS/DZ 13, MIA/CO 12... Indonesians also come
  in at NRT (10), HKG, CDG, FRA, SJC, LHR, ARN, DFW: ISP routing, or VPNs.
- **RIDERS reads 0 MS with bots on the same PC.** Each process sets its own clock offset from its
  room pongs (~160 ms round trips here), and the true transit is a few ms, so ages clamp to 0. Finding
  169 measured 22-30 ms between the PC and the Mac; nothing changed there.
- **`online.c`'s environment-driven room (`DHD_NET_CREATE`) makes rooms of 4** (its barrier's
  settings) and the game went quiet after the countdown at boot in one try; the menu flow
  (cross_race.py's pads) was used instead.

## Result

### The log replay (Auto by colo against the old country rule)

Every `/v1/rooms/new` of the 7 days (Workers Logs) through both rules; the cost is the edge's
measured round trip to the hint's two quickest colos.

- **35 of 427 rooms (8%) would run elsewhere**: apac-se → apac-ne 11 (Indonesians and a Malaysian at
  NRT), apac-se → weur 6 / eeur 4 / wnam 4 (Indonesians at CDG, LHR, MRS, FRA, ARN, SJC, DFW), me →
  weur 5 (Iraq: BGW, BSR, neighbours of Kuwait), apac → apac-se 3 (India), apac-ne → weur 1, afr →
  eeur 1.
- For the 29 of them at a measured edge, the creator's edge-to-room leg gets shorter by 54 ms (median;
  0-180 ms; the Indonesians at European and US edges 142-180 ms). Their room is then far for players
  who reach Cloudflare in Singapore: the creator's ping wins, as the owner asked.
- **148 rooms (35%) are South Americans at GRU, GIG or EZE**, still 113-133 ms from any room (enam).
  No hint fixes that; the race ping shows them their races don't go there.

### Pings from this PC (Algeria; test Worker, nettool host + guest, 30 s in the room)

| Pick | Room ran in | SERVER (median of 30 s) | RACE (median, worst) |
|---|---|---|---|
| Auto | LHR (EU WEST; the edge was LHR) | 118 ms | 7 ms, 8 |
| EU WEST | CDG | 65 ms | 7 ms, 8 |
| EU EAST | OTP | 122 ms | 9 ms, 10 |
| NA EAST | MIA | 236 ms | 7 ms, 8 |
| NA WEST | DFW | 181 ms | 8 ms, 8 |
| ASIA NE | KIX | 280 ms | 8 ms, 18 |
| ASIA SE | SIN | 249 ms | 9 ms, 21 |
| OCEANIA | BNE | 431 ms | 9 ms, 12 |

Every pick landed in its region. The server ping moves with the room by up to 370 ms; the race ping
doesn't move (7-9 ms). Server pings on this home line swing (the same LHR room: 64 and 118 ms medians
a minute apart; worst values to 2.3 s are TCP stalls).

### The 7-racer race

This PC's game (menu flow: Create A Public Race with Room Region Auto, minimised, counter on) and 6
nettool bots (`--races 1 --echo-ai`, 3 of them sending real snapshots) on the test Worker: room in LHR
(EU WEST), 7 racers' Realtime set up (14 channels, released 2.1 s after the start message), the
countdown, the race, every finish, results for all 7 plus 3 AI. The game's counter in the room:
`SERVER 147-164 MS` / `RACE 8-9 MS`; in the race `SERVER 151-166 MS` / `RIDERS 0 MS` (see Surprises).
The room header read `PLAYERS 1/10 - EU WEST SERVER`, the ONLINE row `Room Region: < Auto >`.

### Checks

- `online/server`: `npm test` (174 tests, the table's freshness first) and `npm run typecheck`.
- `cargo test`: dhd_net `a_connected_association_measures_the_race_ping`, dhd_render
  `online_lines_name_what_they_measure_and_fit`, dhd_settings.
- `port/net/build_tests.py`: test_session's area, pick, list and race-ping samples; all pass.
- `python tools/check.py port`: OK. State traces against the shared 0.7.13 reference (db3702d release
  `dhd.exe`, `--env DHD_LAUNCHER=1`): boot 600 and race 5000 rows all match.

### Not done, left for the lead

- No Mac or Linux run (the Mac is with the P2P lane); the change is shared C and Rust.
- Not measured from South America, Africa or the Middle East with a real game; Globalping data
  centres stand in for players.
- The table ages: Cloudflare adds colos and object locations. Refresh with `node
  tools/do_locations.mjs fetch` and new measurements (the method above); a colo not in the table falls
  back to the old rule, so a stale table is never worse than 0.7.13.
- After deploying, the server logs `room CODE: runs in COLO, made with HINT (how) from EDGE` once per
  room: the real placements to check the table against.
