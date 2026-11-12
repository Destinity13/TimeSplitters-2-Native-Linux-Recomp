// TimeSplitters 2 Native - Online-Server (Relay / virtuelles LAN)
//
// Alle Spieler verbinden sich mit diesem Server. Jeder bekommt eine virtuelle IP (10.13.x.y); der Server leitet
// die LAN-Pakete des Spiels zwischen den Spielern weiter. Das Spiel selbst sieht ein normales LAN: Spiele hosten,
// Spiele in der Liste finden, beitreten - wie im Original mit Netzwerkadapter.
//
//   ts2-server [--port 27713] [--name "Mein Server"] [--max 16] [--motd "Text"] [--password geheim]
//
// Ein Port fuer TCP (Steuerung, Spiel-TCP, Ersatzweg fuer UDP) und UDP (Spiel-UDP mit geringer Latenz).
// Keine Abhaengigkeiten, laeuft auf jedem Linux (auch im Container / auf dem Homeserver).
//
// Lizenz: GPL-3.0 (wie der Rest des Ports).

#ifdef _WIN32
#define _CRT_RAND_S
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
using nfds_t = ULONG;
static int poll(pollfd *p, nfds_t n, int ms) { return WSAPoll(p, n, ms); }
static int netzSchliessen(int s) { return closesocket(SOCKET(s)); }
static void nichtBlockierend(int s) { u_long an = 1; ioctlsocket(SOCKET(s), FIONBIO, &an); }
#define MSG_NOSIGNAL 0
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
static int netzSchliessen(int s) { return close(s); }
static void nichtBlockierend(int s) { fcntl(s, F_SETFL, O_NONBLOCK); }
#endif

namespace
{
    // ---- Protokoll (muss zu ts2_override.cpp passen) -------------------------------------------------------------
    constexpr uint16_t kProtokoll = 1;
    constexpr uint32_t kNetz = 0x0A0D0000u;      // 10.13.0.0/16
    constexpr uint32_t kBroadcast = 0x0A0DFFFFu; // 10.13.255.255
    enum : uint8_t
    {
        M_HELLO = 1,      // C->S  u16 protokoll, str name, [str passwort nach \0]
        M_WELCOME = 2,    // S->C  u32 vip, u32 token, str servername \0 motd
        M_REJECT = 3,     // S->C  str grund
        M_DGRAM = 10,     // C->S  u16 srcport, u32 dstvip, u16 dstport, data | S->C u32 srcvip, u16 srcport, u16 dstport, data
        M_SCONNECT = 20,  // C->S  u32 cid, u16 srcport, u32 dstvip, u16 dstport
        M_SINCOMING = 21, // S->C  u32 sid, u32 srcvip, u16 srcport, u16 dstport
        M_SACCEPT = 22,   // C->S  u32 sid, u8 ok
        M_SRESULT = 23,   // S->C  u32 cid, u32 sid, u8 ok
        M_SDATA = 24,     // beide  u32 sid, data
        M_SCLOSE = 25,    // beide  u32 sid
        M_PING = 30,      // C->S  u32 dstvip, u16 id, u32 t, u8 antwort | S->C u32 srcvip, u16 id, u32 t, u8 antwort
        M_KEEPALIVE = 40,
        M_INFO = 41,      // S->C  u32 spielerzahl (nach Beitritt/Abgang)
    };

    // Zufall fuer Sitzungs-Token (nicht rand(): der waere aus Startzeit und PID erratbar)
    uint32_t zufall32()
    {
#ifdef _WIN32
        unsigned r = 0;
        if (rand_s(&r) == 0 && r) return uint32_t(r); // CryptGenRandom-basiert, keine zusaetzliche Bibliothek
#else
        uint32_t v = 0;
        if (FILE *f = std::fopen("/dev/urandom", "rb")) { const size_t n = std::fread(&v, 1, sizeof v, f); std::fclose(f); if (n == sizeof v && v) return v; }
#endif
        return uint32_t(std::rand()) ^ (uint32_t(std::rand()) << 16) ^ uint32_t(std::time(nullptr));
    }

    // Schutzgrenzen (Missbrauch, nicht normales Spiel: 16 Spieler erzeugen zusammen wenige hundert Pakete/s)
    constexpr int kMaxOffen = 64;              // gleichzeitige TCP-Verbindungen insgesamt (angemeldet + wartend)
    constexpr int kMaxJeIp = 4;                // Verbindungen je Quelladresse
    constexpr uint32_t kMaxPaketeJeS = 600;    // weitergeleitete Pakete je Spieler und Sekunde
    constexpr uint32_t kMaxBytesJeS = 1u << 20; // weitergeleitete Bytes je Spieler und Sekunde (1 MiB)
    constexpr size_t kMaxStroemeJeSpieler = 32; // offene/wartende TCP-Stroeme je Spieler
    constexpr uint32_t kMaxAbfragenJeS = 40;   // Serverlisten-Abfragen (T2IN) je Sekunde insgesamt (Reflexionsschutz)

    volatile sig_atomic_t g_ende = 0;
    void beenden(int) { g_ende = 1; }

    void logf(const char *fmt, ...)
    {
        char zeit[32];
        const std::time_t t = std::time(nullptr);
        std::strftime(zeit, sizeof zeit, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::fprintf(stdout, "[%s] ", zeit);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stdout, fmt, ap);
        va_end(ap);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    std::string ipText(uint32_t ip)
    {
        char b[20];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        return b;
    }

    void put16(std::string &s, uint16_t v) { s.push_back(char(v)); s.push_back(char(v >> 8)); }
    void put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char(v >> (8 * i))); }
    uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
    uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

    struct Spieler
    {
        int fd = -1;
        uint32_t vip = 0;
        uint32_t token = 0;
        bool angemeldet = false;
        std::string name;
        std::string ein, aus; // TCP-Puffer
        sockaddr_storage udp{};
        socklen_t udpLen = 0;
        bool hatUdp = false;
        std::time_t zuletzt = 0;
        std::string adresse;
        std::time_t rateSek = 0;   // Sekunde, fuer die pakete/bytes zaehlen
        uint32_t pakete = 0, bytes = 0;
        uint32_t verstoesse = 0;
    };

    struct Strom
    {
        int a = -1, b = -1;   // fds der beiden Enden (a = Verbinder, b = Annehmer)
        uint32_t cid = 0;     // Kennung des Verbinders
        bool offen = false;
    };

    struct Server
    {
        uint16_t port = 27713;
        std::string name = "TS2 Server";
        std::string motd;
        std::string passwort;
        int max = 16;
        int tcp = -1, udp = -1;
        std::map<int, Spieler> spieler;      // fd -> Spieler
        std::map<uint32_t, int> nachVip;     // vip -> fd
        std::map<uint32_t, Strom> stroeme;   // sid -> Strom
        uint32_t naechsteSid = 1;
        std::time_t abfrageSek = 0;
        uint32_t abfragen = 0;

        // Zaehlt ein weitergeleitetes Paket; false = Spieler ueberschreitet die Grenze dauerhaft -> trennen
        bool rateOk(Spieler &s, size_t n)
        {
            const std::time_t jetzt = std::time(nullptr);
            if (s.rateSek != jetzt) { s.rateSek = jetzt; s.pakete = 0; s.bytes = 0; }
            s.pakete++; s.bytes += uint32_t(n);
            if (s.pakete <= kMaxPaketeJeS && s.bytes <= kMaxBytesJeS) return true;
            return ++s.verstoesse < 3000; // kurze Spitzen verwerfen, Dauerflut trennen
        }

        int verbindungenVon(const std::string &adresse) const
        {
            int n = 0;
            for (auto &kv : spieler) n += kv.second.adresse == adresse ? 1 : 0;
            return n;
        }

        void senden(Spieler &s, uint8_t typ, const std::string &nutz)
        {
            const uint32_t len = uint32_t(nutz.size() + 1);
            std::string f;
            put32(f, len);
            f.push_back(char(typ));
            f += nutz;
            if (s.aus.size() > (8u << 20)) return; // haengender Client: nichts mehr puffern
            s.aus += f;
        }

        uint32_t freieVip()
        {
            for (uint32_t i = 2; i < 0xFFFE; ++i)
            {
                const uint32_t v = kNetz | i;
                if ((v & 0xFF) == 0 || (v & 0xFF) == 0xFF) continue;
                if (!nachVip.count(v)) return v;
            }
            return 0;
        }

        int angemeldete() const
        {
            int n = 0;
            for (auto &kv : spieler) n += kv.second.angemeldet ? 1 : 0;
            return n;
        }

        void infoAnAlle()
        {
            std::string p;
            put32(p, uint32_t(angemeldete()));
            for (auto &kv : spieler)
                if (kv.second.angemeldet) senden(kv.second, M_INFO, p);
        }

        void trennen(int fd, const char *grund)
        {
            auto it = spieler.find(fd);
            if (it == spieler.end()) return;
            Spieler &s = it->second;
            if (s.angemeldet)
                logf("Abgang: %s (%s, %s) - %s", s.name.c_str(), ipText(s.vip).c_str(), s.adresse.c_str(), grund);
            // offene Stroeme dieses Spielers beim Gegenueber schliessen
            for (auto st = stroeme.begin(); st != stroeme.end();)
            {
                if (st->second.a == fd || st->second.b == fd)
                {
                    const int anderer = st->second.a == fd ? st->second.b : st->second.a;
                    auto o = spieler.find(anderer);
                    if (o != spieler.end())
                    {
                        std::string p;
                        if (!st->second.offen && anderer == st->second.a)
                        {
                            put32(p, st->second.cid); put32(p, st->first); p.push_back(0);
                            senden(o->second, M_SRESULT, p);
                        }
                        else
                        {
                            put32(p, st->first);
                            senden(o->second, M_SCLOSE, p);
                        }
                    }
                    st = stroeme.erase(st);
                }
                else
                    ++st;
            }
            if (s.vip) nachVip.erase(s.vip);
            const bool warAngemeldet = s.angemeldet;
            netzSchliessen(fd);
            spieler.erase(it);
            if (warAngemeldet) infoAnAlle();
        }

        void datagramm(Spieler &von, uint16_t srcport, uint32_t dstvip, uint16_t dstport, const uint8_t *d, size_t n)
        {
            std::string p;
            put32(p, von.vip);
            put16(p, srcport);
            put16(p, dstport);
            p.append(reinterpret_cast<const char *>(d), n);
            auto zustellen = [&](Spieler &an)
            {
                if (an.hatUdp)
                    sendto(udp, p.data(), int(p.size()), 0, reinterpret_cast<sockaddr *>(&an.udp), an.udpLen);
                else
                    senden(an, M_DGRAM, p);
            };
            const bool bc = dstvip == kBroadcast || dstvip == 0xFFFFFFFFu;
            if (bc)
            {
                for (auto &kv : spieler)
                    if (kv.second.angemeldet && kv.first != von.fd) zustellen(kv.second);
                return;
            }
            auto z = nachVip.find(dstvip);
            if (z != nachVip.end()) zustellen(spieler[z->second]);
        }

        void nachricht(Spieler &s, uint8_t typ, const uint8_t *p, size_t n)
        {
            s.zuletzt = std::time(nullptr);
            if (!s.angemeldet)
            {
                if (typ != M_HELLO || n < 2) { trennen(s.fd, "kein HELLO"); return; }
                const uint16_t proto = get16(p);
                std::string rest(reinterpret_cast<const char *>(p + 2), n - 2);
                std::string name = rest.substr(0, rest.find('\0'));
                std::string pw = rest.find('\0') != std::string::npos ? rest.substr(rest.find('\0') + 1) : "";
                if (name.empty()) name = "Spieler";
                if (name.size() > 24) name.resize(24);
                std::string grund;
                if (proto != kProtokoll) grund = "Falsche Version (Server " + std::to_string(kProtokoll) + ", Spiel " + std::to_string(proto) + ") - bitte aktualisieren";
                else if (!passwort.empty() && pw != passwort) grund = "Falsches Passwort";
                else if (angemeldete() >= max) grund = "Server voll";
                const uint32_t vip = grund.empty() ? freieVip() : 0;
                if (grund.empty() && !vip) grund = "Keine freie Adresse";
                if (!grund.empty())
                {
                    senden(s, M_REJECT, grund);
                    logf("Abgelehnt: %s (%s) - %s", name.c_str(), s.adresse.c_str(), grund.c_str());
                    return; // Verbindung wird nach dem Senden vom Client geschlossen
                }
                s.vip = vip;
                s.name = name;
                s.angemeldet = true;
                s.token = zufall32();
                nachVip[vip] = s.fd;
                std::string w;
                put32(w, vip);
                put32(w, s.token);
                w += this->name;
                w.push_back('\0');
                w += motd;
                senden(s, M_WELCOME, w);
                logf("Beitritt: %s (%s) als %s, %d Spieler", name.c_str(), s.adresse.c_str(), ipText(vip).c_str(), angemeldete());
                infoAnAlle();
                return;
            }
            switch (typ)
            {
            case M_DGRAM:
                if (n < 8) break;
                if (!rateOk(s, n)) { trennen(s.fd, "Paketflut"); return; }
                if (s.pakete <= kMaxPaketeJeS && s.bytes <= kMaxBytesJeS) datagramm(s, get16(p), get32(p + 2), get16(p + 6), p + 8, n - 8);
                break;
            case M_SCONNECT:
            {
                if (n < 12) break;
                const uint32_t cid = get32(p);
                const uint16_t srcport = get16(p + 4);
                const uint32_t dstvip = get32(p + 6);
                const uint16_t dstport = get16(p + 10);
                auto z = nachVip.find(dstvip);
                if (z == nachVip.end() || z->second == s.fd)
                {
                    std::string r;
                    put32(r, cid); put32(r, 0); r.push_back(0);
                    senden(s, M_SRESULT, r);
                    break;
                }
                size_t eigene = 0;
                for (auto &st : stroeme) eigene += (st.second.a == s.fd || st.second.b == s.fd) ? 1 : 0;
                if (eigene >= kMaxStroemeJeSpieler)
                {
                    std::string r;
                    put32(r, cid); put32(r, 0); r.push_back(0);
                    senden(s, M_SRESULT, r);
                    break;
                }
                const uint32_t sid = naechsteSid++;
                stroeme[sid] = Strom{s.fd, z->second, cid, false};
                std::string r;
                put32(r, sid); put32(r, s.vip); put16(r, srcport); put16(r, dstport);
                senden(spieler[z->second], M_SINCOMING, r);
                break;
            }
            case M_SACCEPT:
            {
                if (n < 5) break;
                const uint32_t sid = get32(p);
                auto st = stroeme.find(sid);
                if (st == stroeme.end() || st->second.b != s.fd) break;
                const bool ok = p[4] != 0;
                std::string r;
                put32(r, st->second.cid); put32(r, sid); r.push_back(ok ? 1 : 0);
                auto a = spieler.find(st->second.a);
                if (a != spieler.end()) senden(a->second, M_SRESULT, r);
                if (ok) st->second.offen = true;
                else stroeme.erase(st);
                break;
            }
            case M_SDATA:
            case M_SCLOSE:
            {
                if (n < 4) break;
                const uint32_t sid = get32(p);
                auto st = stroeme.find(sid);
                if (st == stroeme.end() || (st->second.a != s.fd && st->second.b != s.fd)) break;
                if (!rateOk(s, n)) { trennen(s.fd, "Paketflut"); return; }
                const int anderer = st->second.a == s.fd ? st->second.b : st->second.a;
                auto o = spieler.find(anderer);
                if (o != spieler.end() && (typ == M_SCLOSE || (s.pakete <= kMaxPaketeJeS && s.bytes <= kMaxBytesJeS)))
                    senden(o->second, typ, std::string(reinterpret_cast<const char *>(p), n));
                if (typ == M_SCLOSE) stroeme.erase(st);
                break;
            }
            case M_PING:
            {
                if (n < 11) break;
                auto z = nachVip.find(get32(p));
                if (z == nachVip.end()) break;
                std::string r;
                put32(r, s.vip);
                r.append(reinterpret_cast<const char *>(p + 4), n - 4);
                senden(spieler[z->second], M_PING, r);
                break;
            }
            case M_KEEPALIVE:
                break;
            default:
                break;
            }
        }

        void udpLesen()
        {
            uint8_t b[2048];
            for (;;)
            {
                sockaddr_storage von{};
                socklen_t vl = sizeof von;
                const int n = int(recvfrom(udp, reinterpret_cast<char *>(b), sizeof b, 0, reinterpret_cast<sockaddr *>(&von), &vl));
                if (n < 0) return;
                if (n == 8 && std::memcmp(b, "T2IN", 4) == 0) // Serverliste: Abfrage ohne Anmeldung
                {
                    // Reflexionsschutz: die Antwort ist groesser als die Anfrage, also Abfragen je Sekunde begrenzen
                    const std::time_t jetzt = std::time(nullptr);
                    if (abfrageSek != jetzt) { abfrageSek = jetzt; abfragen = 0; }
                    if (++abfragen > kMaxAbfragenJeS) continue;
                    std::string r("T2IN", 4);
                    r.append(reinterpret_cast<const char *>(b + 4), 4);
                    put16(r, kProtokoll);
                    put16(r, uint16_t(angemeldete()));
                    put16(r, uint16_t(max));
                    r.push_back(passwort.empty() ? 0 : 1);
                    r += name;
                    r.push_back('\0');
                    r += motd.substr(0, 200);
                    sendto(udp, r.data(), int(r.size()), 0, reinterpret_cast<sockaddr *>(&von), vl);
                    continue;
                }
                if (n < 12) continue;
                // u32 token, u32 vip, u16 srcport, u32 dstvip, u16 dstport, data  (dstvip 0 = nur Anmeldung)
                const uint32_t token = get32(b), vip = get32(b + 4);
                auto z = nachVip.find(vip);
                if (z == nachVip.end()) continue;
                Spieler &s = spieler[z->second];
                if (s.token != token) continue;
                if (!s.hatUdp || std::memcmp(&s.udp, &von, vl) != 0)
                {
                    s.udp = von;
                    s.udpLen = vl;
                    s.hatUdp = true;
                }
                s.zuletzt = std::time(nullptr);
                if (n < 16)
                {
                    sendto(udp, "T2OK", 4, 0, reinterpret_cast<sockaddr *>(&von), vl);
                    continue;
                }
                const uint32_t dstvip = get32(b + 10);
                if (dstvip == 0) continue;
                if (!rateOk(s, size_t(n))) { trennen(s.fd, "Paketflut"); continue; }
                if (s.pakete <= kMaxPaketeJeS && s.bytes <= kMaxBytesJeS) datagramm(s, get16(b + 8), dstvip, get16(b + 14), b + 16, size_t(n) - 16);
            }
        }

        bool starten()
        {
            // IPv6 mit IPv4-Abbildung (ein Socket fuer beide); ohne IPv6 im System reines IPv4
            for (int v6 = 1; v6 >= 0; --v6)
            {
                sockaddr_storage a{};
                socklen_t al;
                if (v6)
                {
                    auto *x = reinterpret_cast<sockaddr_in6 *>(&a);
                    x->sin6_family = AF_INET6; x->sin6_port = htons(port); x->sin6_addr = in6addr_any; al = sizeof *x;
                }
                else
                {
                    auto *x = reinterpret_cast<sockaddr_in *>(&a);
                    x->sin_family = AF_INET; x->sin_port = htons(port); x->sin_addr.s_addr = htonl(INADDR_ANY); al = sizeof *x;
                }
                const int fam = v6 ? AF_INET6 : AF_INET;
                tcp = int(socket(fam, SOCK_STREAM, 0));
                udp = int(socket(fam, SOCK_DGRAM, 0));
                if (tcp < 0 || udp < 0) { if (tcp >= 0) netzSchliessen(tcp); if (udp >= 0) netzSchliessen(udp); tcp = udp = -1; continue; }
                int nein = 0, ja = 1;
                if (v6)
                {
                    setsockopt(tcp, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&nein), sizeof nein);
                    setsockopt(udp, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&nein), sizeof nein);
                }
                setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&ja), sizeof ja);
                if (bind(tcp, reinterpret_cast<sockaddr *>(&a), al) < 0 || listen(tcp, 32) < 0) { std::perror("TCP-Port"); return false; }
                if (bind(udp, reinterpret_cast<sockaddr *>(&a), al) < 0) { std::perror("UDP-Port"); return false; }
                nichtBlockierend(tcp);
                nichtBlockierend(udp);
                return true;
            }
            std::perror("socket");
            return false;
        }

        void annehmen()
        {
            for (;;)
            {
                sockaddr_storage von{};
                socklen_t vl = sizeof von;
                const int fd = int(accept(tcp, reinterpret_cast<sockaddr *>(&von), &vl));
                if (fd < 0) return;
                nichtBlockierend(fd);
                int ja = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&ja), sizeof ja);
                Spieler s;
                s.fd = fd;
                s.zuletzt = std::time(nullptr);
                char h[INET6_ADDRSTRLEN] = "?";
                if (von.ss_family == AF_INET6)
                    inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(&von)->sin6_addr, h, sizeof h);
                else if (von.ss_family == AF_INET)
                    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&von)->sin_addr, h, sizeof h);
                s.adresse = h;
                if (s.adresse.rfind("::ffff:", 0) == 0) s.adresse = s.adresse.substr(7);
                if (int(spieler.size()) >= kMaxOffen || verbindungenVon(s.adresse) >= kMaxJeIp)
                {
                    netzSchliessen(fd); // ohne Antwort: Verbindungsflut / zu viele von einer Adresse
                    continue;
                }
                spieler[fd] = s;
            }
        }

        void lauf()
        {
            logf("TS2-Server \"%s\" laeuft auf Port %u (TCP+UDP), max. %d Spieler%s", name.c_str(), port, max,
                 passwort.empty() ? "" : ", mit Passwort");
            std::time_t letzterCheck = std::time(nullptr);
            while (!g_ende)
            {
                std::vector<pollfd> pf;
                using fd_t = decltype(pollfd::fd);
                pf.push_back({fd_t(tcp), POLLIN, 0});
                pf.push_back({fd_t(udp), POLLIN, 0});
                for (auto &kv : spieler)
                    pf.push_back({fd_t(kv.first), short(POLLIN | (kv.second.aus.empty() ? 0 : POLLOUT)), 0});
                if (poll(pf.data(), pf.size(), 1000) < 0 && !g_ende) continue;
                if (pf[0].revents & POLLIN) annehmen();
                if (pf[1].revents & POLLIN) udpLesen();
                std::vector<std::pair<int, const char *>> weg;
                for (size_t i = 2; i < pf.size(); ++i)
                {
                    auto it = spieler.find(int(pf[i].fd));
                    if (it == spieler.end()) continue;
                    Spieler &s = it->second;
                    if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { weg.push_back({s.fd, "Verbindung getrennt"}); continue; }
                    if (pf[i].revents & POLLIN)
                    {
                        char b[16384];
                        const int n = int(recv(s.fd, b, sizeof b, 0));
                        if (n <= 0) { weg.push_back({s.fd, "Verbindung getrennt"}); continue; }
                        s.ein.append(b, size_t(n));
                        size_t pos = 0;
                        while (s.ein.size() - pos >= 5)
                        {
                            const uint32_t len = get32(reinterpret_cast<const uint8_t *>(s.ein.data() + pos));
                            if (len == 0 || len > (1u << 20)) { weg.push_back({s.fd, "ungueltige Daten"}); break; }
                            if (s.ein.size() - pos < 4 + len) break;
                            const uint8_t *m = reinterpret_cast<const uint8_t *>(s.ein.data() + pos + 4);
                            const int fd = s.fd;
                            nachricht(s, m[0], m + 1, len - 1);
                            if (!spieler.count(fd)) break;
                            pos += 4 + len;
                        }
                        if (spieler.count(int(pf[i].fd))) s.ein.erase(0, pos);
                    }
                }
                for (auto &w : weg) trennen(w.first, w.second);
                for (auto &kv : spieler)
                {
                    Spieler &s = kv.second;
                    while (!s.aus.empty())
                    {
                        const int n = int(send(s.fd, s.aus.data(), int(s.aus.size()), MSG_NOSIGNAL));
                        if (n <= 0) break;
                        s.aus.erase(0, size_t(n));
                    }
                }
                const std::time_t jetzt = std::time(nullptr);
                if (jetzt - letzterCheck >= 5)
                {
                    letzterCheck = jetzt;
                    std::vector<int> alt;
                    for (auto &kv : spieler)
                        if (jetzt - kv.second.zuletzt > (kv.second.angemeldet ? 30 : 10)) alt.push_back(kv.first);
                    for (int fd : alt) trennen(fd, "Zeitueberschreitung");
                }
            }
            logf("Server beendet");
        }
    };
}

int main(int argc, char **argv)
{
    Server srv;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto wert = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") srv.port = uint16_t(std::atoi(wert().c_str()));
        else if (a == "--name") srv.name = wert();
        else if (a == "--motd") srv.motd = wert();
        else if (a == "--max") srv.max = std::max(2, std::atoi(wert().c_str()));
        else if (a == "--password") srv.passwort = wert();
        else
        {
            std::printf("TimeSplitters 2 Native - Online-Server\n\n"
                        "  %s [--port 27713] [--name \"Mein Server\"] [--max 16] [--motd \"Text\"] [--password geheim]\n\n"
                        "Port fuer TCP und UDP freigeben (Router/Firewall). Spieler tragen im Spiel (ESC-Menue > Online)\n"
                        "die Adresse dieses Rechners ein.\n",
                        argv[0]);
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }
    std::srand(unsigned(std::time(nullptr)) ^ unsigned(getpid()));
    std::signal(SIGINT, beenden);
    std::signal(SIGTERM, beenden);
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::fprintf(stderr, "Winsock nicht verfuegbar\n"); return 1; }
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if (!srv.starten()) return 1;
    srv.lauf();
    return 0;
}
