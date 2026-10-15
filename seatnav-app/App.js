import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  ActivityIndicator,
  FlatList,
  Modal,
  Pressable,
  RefreshControl,
  StyleSheet,
  Text,
  View,
  useColorScheme,
  useWindowDimensions,
} from "react-native";
import { StatusBar } from "expo-status-bar";
import { SafeAreaProvider, SafeAreaView } from "react-native-safe-area-context";

import { API_URL } from "./config";

// ---------- look ----------

const THEMES = {
  light: {
    bg: "#f3f5f2", surface: "#ffffff", text: "#1c231f", muted: "#5d6862", line: "#d9dfda",
    brand: "#00693e", overlay: "rgba(20, 28, 24, 0.45)",
    FREE: "#2e7d4f", OCCUPIED: "#a23b2a", AWAY: "#b7801a", OFFLINE: "#8a918d",
    tint: { FREE: "#e4f1e8", OCCUPIED: "#f6e4e0", AWAY: "#f7eedb", OFFLINE: "#eceeed" },
  },
  dark: {
    bg: "#121614", surface: "#1a201c", text: "#e5eae6", muted: "#9aa69f", line: "#2c3530",
    brand: "#4fb386", overlay: "rgba(0, 0, 0, 0.6)",
    FREE: "#5cbf85", OCCUPIED: "#e2735f", AWAY: "#e0ac48", OFFLINE: "#7d8682",
    tint: { FREE: "#1c2f23", OCCUPIED: "#35201b", AWAY: "#332a17", OFFLINE: "#222826" },
  },
};

const LABEL = { FREE: "Free", OCCUPIED: "Occupied", AWAY: "Away", OFFLINE: "Sensor offline" };

// ---------- helpers ----------

function duration(seconds) {
  if (seconds == null || seconds < 0) return "";
  if (seconds < 60) return "under a minute";
  const m = Math.floor(seconds / 60);
  if (m < 60) return m + (m === 1 ? " minute" : " minutes");
  return Math.floor(m / 60) + " h " + (m % 60) + " min";
}

function clockTime(ts) {
  const d = new Date(ts * 1000);
  return d.getHours().toString().padStart(2, "0") + ":" + d.getMinutes().toString().padStart(2, "0");
}

function signal(rssi) {
  if (rssi == null) return "";
  const word = rssi >= -67 ? "good" : rssi >= -75 ? "fair" : "weak";
  return "Wi-Fi " + word + " (" + rssi + " dBm)";
}

function bySeatId(a, b) {
  return a.seat_id.localeCompare(b.seat_id, undefined, { numeric: true });
}

// ---------- data: initial load + live updates ----------

function useSeats() {
  const [seats, setSeats] = useState({});          // seat_id -> seat
  const [clockOffset, setClockOffset] = useState(0); // server time minus phone time (s)
  const [connection, setConnection] = useState("connecting"); // connecting | live | reconnecting
  const [error, setError] = useState(null);
  const [loaded, setLoaded] = useState(false);

  const applySnapshot = useCallback((data) => {
    setClockOffset(data.server_time - Date.now() / 1000);
    const next = {};
    data.seats.forEach((s) => { next[s.seat_id] = s; });
    setSeats(next);
    setLoaded(true);
  }, []);

  const refresh = useCallback(async () => {
    try {
      const res = await fetch(API_URL + "/api/seats");
      if (!res.ok) throw new Error("the server answered " + res.status);
      applySnapshot(await res.json());
      setError(null);
    } catch (e) {
      setError("Can't reach the SeatNav server at " + API_URL + " (" + e.message + "). " +
               "Check API_URL in config.js and that the server is running.");
    }
  }, [applySnapshot]);

  useEffect(() => {
    let stopped = false;
    let retryTimer = null;
    let ws = null;

    const connect = () => {
      ws = new WebSocket(API_URL.replace(/^http/, "ws") + "/ws");   // http -> ws, https -> wss
      ws.onopen = () => { setConnection("live"); setError(null); };
      ws.onmessage = (event) => {
        const msg = JSON.parse(event.data);
        if (msg.type === "snapshot") applySnapshot(msg);
        if (msg.type === "seat") setSeats((prev) => ({ ...prev, [msg.seat.seat_id]: msg.seat }));
      };
      ws.onerror = () => {};                     // onclose runs right after; handled there
      ws.onclose = () => {
        if (stopped) return;
        setConnection("reconnecting");
        retryTimer = setTimeout(connect, 3000);
      };
    };

    refresh();
    connect();
    return () => {
      stopped = true;
      clearTimeout(retryTimer);
      if (ws) ws.close();
    };
  }, [applySnapshot, refresh]);

  return { seats, clockOffset, connection, error, loaded, refresh };
}

// ---------- screens ----------

export default function App() {
  return (
    <SafeAreaProvider>
      <SeatScreen />
    </SafeAreaProvider>
  );
}

function SeatScreen() {
  const theme = THEMES[useColorScheme() === "dark" ? "dark" : "light"];
  const s = useMemo(() => makeStyles(theme), [theme]);
  const { width } = useWindowDimensions();
  const columns = width >= 1000 ? 4 : width >= 680 ? 3 : 2;

  const { seats, clockOffset, connection, error, loaded, refresh } = useSeats();
  const [selectedId, setSelectedId] = useState(null);
  const [refreshing, setRefreshing] = useState(false);
  const [, setTick] = useState(0);

  // re-render every 15 s so "for X minutes" stays current
  useEffect(() => {
    const t = setInterval(() => setTick((n) => n + 1), 15000);
    return () => clearInterval(t);
  }, []);

  const serverNow = Date.now() / 1000 + clockOffset;
  const list = Object.values(seats).sort(bySeatId);

  // pad the last row with invisible fillers so cards keep the same width
  const fillers = list.length % columns ? columns - (list.length % columns) : 0;
  const gridData = list.length
    ? [...list, ...Array.from({ length: fillers }, (_, i) => ({ seat_id: "__filler-" + i, filler: true }))]
    : [];
  const count = { FREE: 0, OCCUPIED: 0, AWAY: 0, OFFLINE: 0 };
  list.forEach((seat) => { count[seat.display_state]++; });
  const reporting = list.length - count.OFFLINE;

  const onRefresh = async () => {
    setRefreshing(true);
    await refresh();
    setRefreshing(false);
  };

  const header = (
    <View style={s.headerBlock}>
      <View style={s.topRow}>
        <Text style={s.brand}>SeatNav</Text>
        <View style={s.connRow}>
          <View style={[s.dot, { backgroundColor: connection === "live" ? theme.FREE : theme.OFFLINE }]} />
          <Text style={s.muted}>{connection === "live" ? "Live" : connection === "connecting" ? "Connecting…" : "Reconnecting…"}</Text>
        </View>
      </View>

      {error ? <Text style={s.error}>{error}</Text> : null}

      {list.length > 0 ? (
        <>
          <Text style={s.hero}>
            <Text style={s.heroCount}>{count.FREE}</Text>
            {" of " + reporting + (reporting === 1 ? " seat" : " seats") + " free"}
          </Text>
          <Text style={s.tally}>
            {count.OCCUPIED} occupied, {count.AWAY} away
            {count.OFFLINE ? ", " + count.OFFLINE + " sensor" + (count.OFFLINE === 1 ? "" : "s") + " offline" : ""}
          </Text>
        </>
      ) : null}
    </View>
  );

  const empty = loaded ? (
    <View style={s.emptyBox}>
      <Text style={s.heroSmall}>No seats yet</Text>
      <Text style={s.muted}>
        Waiting for the first sensor to report. Power on a SeatNav sensor, or run
        "python simulate.py" next to the backend to add test seats.
      </Text>
    </View>
  ) : error ? null : (
    <ActivityIndicator style={{ marginTop: 48 }} color={theme.brand} />
  );

  return (
    <SafeAreaView style={s.screen} edges={["top", "left", "right"]}>
      <StatusBar style={theme === THEMES.dark ? "light" : "dark"} />
      <FlatList
        key={columns}                               // FlatList needs a new key when columns change
        data={gridData}
        keyExtractor={(seat) => seat.seat_id}
        numColumns={columns}
        columnWrapperStyle={columns > 1 ? s.columnGap : undefined}
        contentContainerStyle={s.content}
        ListHeaderComponent={header}
        ListEmptyComponent={empty}
        refreshControl={<RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={theme.brand} />}
        renderItem={({ item }) => item.filler ? (
          <View style={s.filler} />
        ) : (
          <SeatCard seat={item} serverNow={serverNow} theme={theme} s={s} onPress={() => setSelectedId(item.seat_id)} />
        )}
      />

      <SeatDetail
        seat={selectedId ? seats[selectedId] : null}
        serverNow={serverNow}
        theme={theme}
        s={s}
        onClose={() => setSelectedId(null)}
      />
    </SafeAreaView>
  );
}

function SeatCard({ seat, serverNow, theme, s, onPress }) {
  const st = seat.display_state;
  const offline = st === "OFFLINE";
  const since = offline
    ? (seat.last_seen ? "Last heard " + duration(serverNow - seat.last_seen) + " ago" : "No recent data")
    : (seat.state_since ? "For " + duration(serverNow - seat.state_since) : "");

  return (
    <Pressable
      onPress={onPress}
      accessibilityRole="button"
      accessibilityLabel={seat.seat_id + ", " + LABEL[st] + (since ? ", " + since : "")}
      style={({ pressed }) => [s.card, { borderTopColor: theme[st], opacity: pressed ? 0.75 : 1 }]}
    >
      <Text style={s.cardTitle}>{seat.seat_id}</Text>
      <View style={[s.pill, { backgroundColor: theme.tint[st] }]}>
        <Text style={[s.pillText, { color: theme[st] }]}>{LABEL[st]}</Text>
      </View>
      {since ? <Text style={s.small}>{since}</Text> : null}
      {!offline && seat.rssi != null ? <Text style={[s.small, { marginTop: 6 }]}>{signal(seat.rssi)}</Text> : null}
    </Pressable>
  );
}

function SeatDetail({ seat, serverNow, theme, s, onClose }) {
  const [usage, setUsage] = useState(null);
  const [changes, setChanges] = useState(null);
  const [error, setError] = useState(null);
  const seatId = seat ? seat.seat_id : null;
  const state = seat ? seat.display_state : null;

  // load (and reload when the seat changes state) today's usage + recent changes
  useEffect(() => {
    if (!seatId) return;
    let cancelled = false;
    const base = API_URL + "/api/seats/" + encodeURIComponent(seatId);
    Promise.all([
      fetch(base + "/usage?hours=24").then((r) => r.json()),
      fetch(base + "/history?hours=24").then((r) => r.json()),
    ])
      .then(([u, h]) => {
        if (cancelled) return;
        setUsage(u);
        setChanges(h.events.filter((e) => e.kind === "state").slice(-8).reverse());
        setError(null);
      })
      .catch((e) => { if (!cancelled) setError("Couldn't load this seat's history (" + e.message + ")."); });
    return () => { cancelled = true; };
  }, [seatId, state]);

  useEffect(() => {
    if (!seatId) { setUsage(null); setChanges(null); setError(null); }
  }, [seatId]);

  return (
    <Modal visible={!!seat} transparent animationType="slide" onRequestClose={onClose}>
      <Pressable style={[s.overlay, { backgroundColor: theme.overlay }]} onPress={onClose} accessibilityLabel="Close">
        <Pressable style={s.sheet} onPress={() => {}}>
          {seat ? (
            <>
              <View style={s.topRow}>
                <Text style={s.sheetTitle}>{seat.seat_id}</Text>
                <Pressable onPress={onClose} accessibilityRole="button" hitSlop={12}>
                  <Text style={[s.link, { color: theme.brand }]}>Close</Text>
                </Pressable>
              </View>

              <View style={[s.pill, { backgroundColor: theme.tint[seat.display_state], marginTop: 12 }]}>
                <Text style={[s.pillText, { color: theme[seat.display_state] }]}>{LABEL[seat.display_state]}</Text>
              </View>
              {seat.display_state !== "OFFLINE" && seat.state_since ? (
                <Text style={s.small}>For {duration(serverNow - seat.state_since)}</Text>
              ) : null}

              <Text style={s.section}>Last 24 hours</Text>
              {error ? <Text style={s.error}>{error}</Text> : null}
              {!usage && !error ? <ActivityIndicator color={theme.brand} /> : null}
              {usage ? (
                <>
                  <View style={s.bar}>
                    {["OCCUPIED", "AWAY", "FREE"].map((k) =>
                      usage.percent[k] > 0 ? (
                        <View key={k} style={{ flex: usage.percent[k], backgroundColor: theme[k] }} />
                      ) : null
                    )}
                  </View>
                  <Text style={s.small}>
                    Occupied {usage.percent.OCCUPIED}%, away {usage.percent.AWAY}%, free {usage.percent.FREE}%
                  </Text>
                </>
              ) : null}

              <Text style={s.section}>Recent changes</Text>
              {changes && changes.length === 0 ? <Text style={s.small}>No changes in the last 24 hours.</Text> : null}
              {changes
                ? changes.map((e, i) => (
                    <View key={e.ts + "-" + i} style={s.changeRow}>
                      <Text style={s.changeTime}>{clockTime(e.ts)}</Text>
                      <View style={[s.dot, { backgroundColor: theme[e.value] }]} />
                      <Text style={s.changeText}>{LABEL[e.value]}</Text>
                    </View>
                  ))
                : null}
            </>
          ) : null}
        </Pressable>
      </Pressable>
    </Modal>
  );
}

// ---------- styles ----------

function makeStyles(t) {
  return StyleSheet.create({
    screen: { flex: 1, backgroundColor: t.bg },
    content: { padding: 20, paddingBottom: 48, maxWidth: 1000, width: "100%", alignSelf: "center" },
    headerBlock: { marginBottom: 24 },
    topRow: { flexDirection: "row", justifyContent: "space-between", alignItems: "center" },
    brand: { color: t.brand, fontWeight: "600", fontSize: 16 },
    connRow: { flexDirection: "row", alignItems: "center", gap: 8 },
    dot: { width: 8, height: 8, borderRadius: 4 },
    muted: { color: t.muted, fontSize: 14, lineHeight: 20 },
    error: { color: t.OCCUPIED, fontSize: 14, lineHeight: 20, marginTop: 16 },
    hero: { color: t.text, fontSize: 40, lineHeight: 46, marginTop: 28 },
    heroCount: { color: t.FREE, fontWeight: "700" },
    heroSmall: { color: t.text, fontSize: 24, marginBottom: 8 },
    tally: { color: t.muted, fontSize: 15, marginTop: 6 },
    emptyBox: { marginTop: 8, maxWidth: 520 },

    columnGap: { gap: 12 },
    card: {
      flex: 1,
      backgroundColor: t.surface,
      borderWidth: 1,
      borderColor: t.line,
      borderTopWidth: 6,
      borderTopLeftRadius: 4,
      borderTopRightRadius: 4,
      borderBottomLeftRadius: 10,
      borderBottomRightRadius: 10,
      padding: 14,
      marginBottom: 12,
      minHeight: 120,
    },
    filler: { flex: 1, marginBottom: 12 },
    cardTitle: { color: t.text, fontWeight: "600", fontSize: 15 },
    pill: { alignSelf: "flex-start", borderRadius: 999, paddingHorizontal: 10, paddingVertical: 2, marginTop: 10, marginBottom: 4 },
    pillText: { fontWeight: "600", fontSize: 15 },
    small: { color: t.muted, fontSize: 13, lineHeight: 18 },

    overlay: { flex: 1, justifyContent: "flex-end" },
    sheet: {
      backgroundColor: t.surface,
      borderTopLeftRadius: 16,
      borderTopRightRadius: 16,
      padding: 20,
      paddingBottom: 40,
      width: "100%",
      maxWidth: 560,
      alignSelf: "center",
    },
    sheetTitle: { color: t.text, fontSize: 22, fontWeight: "600" },
    link: { fontSize: 16, fontWeight: "600" },
    section: { color: t.text, fontSize: 15, fontWeight: "600", marginTop: 24, marginBottom: 8 },
    bar: { flexDirection: "row", height: 12, borderRadius: 6, overflow: "hidden", backgroundColor: t.line, marginBottom: 6 },
    changeRow: { flexDirection: "row", alignItems: "center", gap: 10, paddingVertical: 6 },
    changeTime: { color: t.muted, fontSize: 14, width: 48, fontVariant: ["tabular-nums"] },
    changeText: { color: t.text, fontSize: 14 },
  });
}
