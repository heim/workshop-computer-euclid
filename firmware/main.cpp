// Euclid4 — 4-kanals euklidsk trigger generator for Workshop Computer
// Bygget på Chris Johnsons ComputerCard-bibliotek (v0.3.x)
//
// To uavhengige euklidske generatorer (A og B) + komplementene = 4 triggerspor.
//
// I/O-mapping:
//   Pulse In 1   : klokke (ekstern, felles for A og B)
//   Pulse In 2   : reset (rising edge -> begge til steg 0)
//   Pulse Out 1  : kanal A, euklidsk mønster
//   Pulse Out 2  : kanal B, euklidsk mønster
//   Audio Out 1  : kanal A, komplement (stegene som IKKE fyrer)
//   Audio Out 2  : kanal B, komplement
//   CV Out 1     : S&H stepped random, samples ny verdi på hver A-trigger
//   CV Out 2     : S&H stepped random, samples ny verdi på hver B-trigger
//   Main knob    : fills (0..steps) for valgt kanal
//   Knob X       : steps (1..16) for valgt kanal
//   Knob Y       : rotasjon (0..steps-1) for valgt kanal
//   Switch Up    : rediger kanal A
//   Switch Down  : rediger kanal B
//   Switch Middle: låst (performance-modus, knottene gjør ingenting)
//   LED 0 / 1    : A-trigger / B-trigger
//   LED 2 / 3    : A-komplement / B-komplement
//   LED 4 / 5    : viser redigeringsmodus (4 = A, 5 = B, begge av = låst)
//
// Knottene har soft pickup: etter bytte av redigeringskanal er en knott
// inaktiv til den flyttes, så verdier ikke hopper når du flipper bryteren.
//
// ---------------------------------------------------------------------------
// USB CDC-styring (se README):
//
//   ComputerCard::Run() blokkerer core0 og kjører hele audio-DSP-en i en
//   interrupt på core0. Vi legger derfor HELE USB-stacken (rå TinyUSB) på
//   core1 via multicore_launch_core1(). Da fyrer USBCTRL_IRQ på core1 og
//   rører aldri det 48 kHz audio-interruptet på core0 — "audio-interruptet
//   er hellig". (pico_stdio_usb ble valgt bort fordi dens tud_task()-timer
//   havner i default alarm-pool på core0 og ville kjempet med audioen.)
//
//   Datautveksling mellom kjernene er låsefri:
//     core1 -> core0 : parameter-mailbox med dirty-flag (les kommentar under)
//     core0 -> core1 : publisert tilstands-snapshot beskyttet av en seqlock
//   Ingen av delene tar en lås i audio-interruptet; core0 bare skriver noen
//   volatile-felter og øker en teller.
//
//   Tekstprotokoll (linjebasert, én kommando per linje, \n-terminert):
//     SET <A|B> <steps|fills|rot> <verdi>   sett parameter
//     GET                                   svar med full tilstand som JSON
//     RESET                                 begge kanaler til steg 0
//   Uoppfordret push: samme JSON-linje sendes ved hver tilstandsendring
//   (klokkepuls eller redigering), strupet til maks ~30 meldinger/sek.

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ComputerCard.h"
#include "tusb.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

// Firmware-versjon. Settes normalt av byggesystemet (-DEUCLID_VERSION=...
// fra release-workflowen), ellers "dev". Rapporteres i JSON-tilstanden.
#ifndef EUCLID_VERSION
#define EUCLID_VERSION "dev"
#endif

// Data-minnebarriere. RP2040 (Cortex-M0+) kjører in-order, men vi bruker en
// eksplisitt dmb + "memory"-clobber slik at kompilatoren ikke omorganiserer
// rekkefølgen på verdi/dirty-skrivene i mailboxen eller seqlock-en.
static inline void mem_barrier()
{
#if defined(__arm__) || defined(__ARM_ARCH)
	__asm volatile("dmb" ::: "memory");
#else
	__asm volatile("" ::: "memory");   // host-bygg (tester): kun kompilator-barriere
#endif
}

struct EuclidChannel
{
	int steps = 16;
	int fills = 4;
	int rotation = 0;
	int currentStep = 0;
	uint32_t pattern = 0;

	void Recalc()
	{
		uint32_t p = 0;
		if (fills > 0)
		{
			int prev = -1;
			for (int i = 0; i < steps; i++)
			{
				int cur = (i * fills) / steps;
				if (cur != prev) p |= (1u << i);
				prev = cur;
			}
		}
		if (rotation > 0 && steps > 0)
		{
			uint32_t mask = (steps >= 32) ? 0xFFFFFFFFu : ((1u << steps) - 1);
			p = ((p << rotation) | (p >> (steps - rotation))) & mask;
		}
		pattern = p;
	}

	// Returnerer true hvis gjeldende steg fyrer, og går videre
	bool Advance()
	{
		bool hit = (pattern >> currentStep) & 1;
		currentStep++;
		if (currentStep >= steps) currentStep = 0;
		return hit;
	}
};

// ===========================================================================
// Delt tilstand mellom core0 (audio) og core1 (USB)
// ===========================================================================
namespace shared
{
	enum { CH_A = 0, CH_B = 1 };

	// --- core1 -> core0: parameter-skrive-mailbox (dirty-flag-mønster) ---
	//
	// Én luke per (kanal, parameter). Produsenten (core1) skriver 'val' og
	// setter deretter 'dirty'. Konsumenten (core0, i audio-IRQ) NULLER 'dirty'
	// FØR den leser 'val' — da kan ingen oppdatering gå tapt: hvis core1 rekker
	// å skrive en ny verdi i vinduet, står 'dirty' fortsatt igjen som 1 og
	// verdien plukkes opp på neste sample. Alle aksesser er enkle, justerte
	// 16/8-bits volatile-lagringer (atomære på RP2040-bussen); mem_barrier()
	// ordner val/dirty-paret.
	struct ParamSlot { volatile int16_t val; volatile uint8_t dirty; };
	ParamSlot reqSteps[2] = {};
	ParamSlot reqFills[2] = {};
	ParamSlot reqRot[2]   = {};
	volatile uint8_t reqReset = 0;   // RESET-kommando

	// --- core0 -> core1: publisert tilstands-snapshot (seqlock) ---
	//
	// core0 skriver snapshotet mellom to økninger av 'snapSeq'. Partallsverdi
	// = stabilt, oddetall = skriving pågår. core1 leser feltene og sjekker at
	// 'snapSeq' er uendret og partall rundt lesingen; ellers prøver den på
	// nytt. Skriveren (audio-IRQ) venter aldri.
	struct ChanSnap { int16_t steps, fills, rot, step; uint32_t pattern; };
	struct Snapshot { ChanSnap ch[2]; };
	volatile uint32_t snapSeq = 0;
	Snapshot snapData = {};

	// Kalles fra core0 (audio-IRQ) ved hver tilstandsendring.
	inline void PublishSnapshot(const EuclidChannel &a, const EuclidChannel &b)
	{
		uint32_t s = snapSeq + 1u;
		snapSeq = s;               // oddetall -> skriving pågår
		mem_barrier();
		snapData.ch[CH_A] = { (int16_t)a.steps, (int16_t)a.fills,
		                      (int16_t)a.rotation, (int16_t)a.currentStep, a.pattern };
		snapData.ch[CH_B] = { (int16_t)b.steps, (int16_t)b.fills,
		                      (int16_t)b.rotation, (int16_t)b.currentStep, b.pattern };
		mem_barrier();
		snapSeq = s + 1u;          // partall -> stabilt igjen
	}

	// Kalles fra core1. Returnerer et konsistent snapshot + sekvensnummeret.
	inline bool ReadSnapshot(Snapshot &out, uint32_t &seqOut)
	{
		for (int i = 0; i < 16; i++)
		{
			uint32_t s1 = snapSeq;
			if (s1 & 1u) continue;     // skriving pågår
			mem_barrier();
			out = snapData;
			mem_barrier();
			uint32_t s2 = snapSeq;
			if (s1 == s2) { seqOut = s1; return true; }
		}
		return false;
	}
}

class Euclid4Card : public ComputerCard
{
public:
	Euclid4Card()
	{
		chA.steps = 16; chA.fills = 4;  chA.rotation = 0; chA.Recalc();
		chB.steps = 16; chB.fills = 7;  chB.rotation = 3; chB.Recalc();
		rngState = 0x2A5F1E3Du;

		// Publiser starttilstand slik at en GET rett etter tilkobling
		// (før noen klokkepuls) svarer med riktige verdier.
		shared::PublishSnapshot(chA, chB);
	}

	virtual void ProcessSample() override
	{
		bool changed = false;
		sampleCounter++;

		// Tap tempo: en flikk ned i Down-posisjon registrerer et tap
		Switch sw = SwitchVal();
		if (sw == Down && lastSwitchPos != Down) RegisterTap();
		lastSwitchPos = sw;

		// USB-skriv (core1 -> core0) plukkes opp først. Deretter kjører
		// knott-håndteringen; "sist skrevne verdi vinner" (knott eller USB)
		// faller naturlig ut av rekkefølgen — flytter du en fysisk knott
		// overtar den, ellers står USB-verdien.
		changed |= ApplyUsbMailbox();
		changed |= HandleEditing();

		if (PulseIn2RisingEdge())
		{
			chA.currentStep = 0;
			chB.currentStep = 0;
			internalCounter = 0;   // synk intern klokke til reset
			changed = true;
		}

		// Klokkekilde: ekstern (Pulse In 1) overstyrer den interne tap-klokka.
		// Kommer det eksterne kanter, brukes de; ellers løper intern-klokka fritt.
		bool tick = false;
		if (PulseIn1RisingEdge())
		{
			lastExtEdge = sampleCounter;
			extEverSeen = true;
			internalCounter = 0;   // hold intern fase nullet mens ekstern styrer
			tick = true;
		}
		else if (extEverSeen && (uint32_t)(sampleCounter - lastExtEdge) < kExtClockTimeout)
		{
			internalCounter = 0;   // ekstern klokke fortsatt aktiv -> intern undertrykt
		}
		else if (++internalCounter >= internalPeriod)
		{
			internalCounter = 0;
			tick = true;           // intern klokke-tick
		}

		if (tick)
		{
			bool hitA = chA.Advance();
			bool hitB = chB.Advance();

			if (hitA)
			{
				PulseOut1(true);
				trigA = kTrigSamples;
				CVOut1(NextRandom());   // S&H: ny tilfeldig verdi, holdes til neste A-trigger
			}
			else
			{
				compA = kTrigSamples;
			}

			if (hitB)
			{
				PulseOut2(true);
				trigB = kTrigSamples;
				CVOut2(NextRandom());   // S&H på B-triggere
			}
			else
			{
				compB = kTrigSamples;
			}

			changed = true;   // steget gikk videre -> push til web-UI
		}

		// Triggerlengde 10 ms, audio-outs som triggerkanaler for komplementene
		if (trigA > 0 && --trigA == 0) PulseOut1(false);
		if (trigB > 0 && --trigB == 0) PulseOut2(false);

		AudioOut1(compA > 0 ? 2047 : 0);
		AudioOut2(compB > 0 ? 2047 : 0);
		if (compA > 0) compA--;
		if (compB > 0) compB--;

		// LEDs 0-3: A/B-trigger og A/B-komplement
		LedOn(0, trigA > 0);
		LedOn(1, trigB > 0);
		LedOn(2, compA > 0);
		LedOn(3, compB > 0);

		// I tap-modus (Down) blinker redigerings-LED-ene ved hvert tap
		if (sw == Down)
		{
			bool f = tapFlash > 0;
			LedOn(4, f);
			LedOn(5, f);
		}
		if (tapFlash > 0) tapFlash--;

		// Publiser til core1 (for GET/push) når noe endret seg. core1 oppdager
		// endringen via snapSeq og sender (strupet) én JSON-linje.
		if (changed) shared::PublishSnapshot(chA, chB);
	}

private:
	static constexpr int kTrigSamples = 480; // 10 ms @ 48 kHz
	static constexpr int kMaxSteps = 16;
	static constexpr int32_t kPickupThreshold = 100; // av 4096

	// Intern klokke (fritt løpende når ingen ekstern klokke er aktiv).
	// Én tick = ett steg. Tap tempo setter perioden (samples mellom steg).
	static constexpr uint32_t kDefaultStepSamples = 12000; // 250 ms/steg @ 48 kHz
	static constexpr uint32_t kMinStepSamples     = 2400;  // ~50 ms  (raskeste tap)
	static constexpr uint32_t kMaxStepSamples     = 96000; // ~2 s    (tregeste tap)
	static constexpr uint32_t kExtClockTimeout    = 120000;// 2.5 s uten ekstern kant -> intern overtar

	EuclidChannel chA, chB;
	int trigA = 0, trigB = 0, compA = 0, compB = 0;
	uint32_t rngState;

	// Soft pickup-tilstand
	Switch lastEditSwitch = Middle;
	bool editFrozenOnce = false;       // sikrer at knottene fryses ved første redigering
	int32_t knobRef[3] = {-1, -1, -1}; // posisjon ved kanalbytte, -1 = aktiv
	int lastZone[3] = {0, 0, 0};
	int knobOut[3] = {-1, -1, -1};     // sist verdi hver knott faktisk drev (fills,steps,rot)

	// Klokke-/tap-tilstand
	uint32_t sampleCounter = 0;                 // fritt løpende sample-teller
	uint32_t internalPeriod = kDefaultStepSamples;
	uint32_t internalCounter = 0;
	uint32_t lastExtEdge = 0;                   // sampleCounter ved siste Pulse In 1-kant
	bool extEverSeen = false;
	uint32_t lastTap = 0;                       // sampleCounter ved forrige tap
	bool tapValid = false;
	int tapFlash = 0;                           // LED-blink ved tap
	Switch lastSwitchPos = Middle;              // for å oppdage flikk til Down

	// xorshift32, skalert til -2047..2047 for CVOut
	int16_t NextRandom()
	{
		rngState ^= rngState << 13;
		rngState ^= rngState >> 17;
		rngState ^= rngState << 5;
		return (int16_t)((rngState & 0xFFF) - 2048);
	}

	// Plukker opp ventende USB-skriv fra mailboxen. Returnerer true hvis noe
	// endret seg (så ProcessSample vet at den skal publisere).
	bool ApplyUsbMailbox()
	{
		bool changed = false;

		if (shared::reqReset)
		{
			shared::reqReset = 0;
			mem_barrier();
			chA.currentStep = 0;
			chB.currentStep = 0;
			changed = true;
		}

		EuclidChannel *chs[2] = { &chA, &chB };
		for (int c = 0; c < 2; c++)
		{
			EuclidChannel &ch = *chs[c];
			bool chChanged = false;

			// Nullstill dirty før vi leser val (se mailbox-kommentaren).
			if (shared::reqSteps[c].dirty)
			{
				shared::reqSteps[c].dirty = 0;
				mem_barrier();
				int v = shared::reqSteps[c].val;
				if (v < 1) v = 1;
				if (v > kMaxSteps) v = kMaxSteps;   // valider steps 1..16
				if (v != ch.steps) { ch.steps = v; chChanged = true; }
			}
			if (shared::reqFills[c].dirty)
			{
				shared::reqFills[c].dirty = 0;
				mem_barrier();
				int v = shared::reqFills[c].val;
				if (v < 0) v = 0;                    // øvre grense klemmes mot steps under
				if (v != ch.fills) { ch.fills = v; chChanged = true; }
			}
			if (shared::reqRot[c].dirty)
			{
				shared::reqRot[c].dirty = 0;
				mem_barrier();
				int v = shared::reqRot[c].val;
				if (v < 0) v = 0;
				if (v != ch.rotation) { ch.rotation = v; chChanged = true; }
			}

			if (chChanged)
			{
				// Klem relative grenser: fills 0..steps, rot 0..steps-1
				if (ch.fills > ch.steps) ch.fills = ch.steps;
				if (ch.rotation > ch.steps - 1) ch.rotation = ch.steps - 1;
				if (ch.rotation < 0) ch.rotation = 0;
				ch.Recalc();
				if (ch.currentStep >= ch.steps) ch.currentStep = 0;
				changed = true;
			}
		}
		return changed;
	}

	int QuantizeKnob(Knob k, int maxVal, int zoneIdx)
	{
		int32_t raw = KnobVal(k);
		int zones = maxVal + 1;
		int val = (raw * zones) >> 12;
		if (val > maxVal) val = maxVal;

		int32_t zoneSize = 4096 / zones;
		int32_t center = lastZone[zoneIdx] * zoneSize + zoneSize / 2;
		if (val != lastZone[zoneIdx])
		{
			int32_t dist = raw - center;
			if (dist < 0) dist = -dist;
			if (dist > zoneSize / 2 + zoneSize / 8)
				lastZone[zoneIdx] = val;
		}
		if (lastZone[zoneIdx] > maxVal) lastZone[zoneIdx] = maxVal;
		return lastZone[zoneIdx];
	}

	// Returnerer true hvis en knott endret en parameter denne samplen.
	bool HandleEditing()
	{
		Switch sw = SwitchVal();

		// Down = tap tempo: knottene redigerer ikke (LED 4/5 styres i ProcessSample)
		if (sw == Down)
		{
			lastEditSwitch = Down;
			return false;
		}

		// Up = rediger A, Middle = rediger B
		EuclidChannel &ch = (sw == Up) ? chA : chB;
		LedOn(4, sw == Up);
		LedOn(5, sw == Middle);

		// Ved kanalbytte — eller aller første redigering — frys knottene til de
		// flyttes. editFrozenOnce trengs fordi Middle nå redigerer (før returnerte
		// den tidlig), så uten den ville knottene gripe kanal B på første sample.
		if (!editFrozenOnce || sw != lastEditSwitch)
		{
			knobRef[0] = KnobVal(Knob::Main);
			knobRef[1] = KnobVal(Knob::X);
			knobRef[2] = KnobVal(Knob::Y);
			lastZone[0] = ch.fills;
			lastZone[1] = ch.steps - 1;
			lastZone[2] = ch.rotation;
			knobOut[0] = ch.fills;
			knobOut[1] = ch.steps;
			knobOut[2] = ch.rotation;
			lastEditSwitch = sw;
			editFrozenOnce = true;
		}

		// Aktiver knott når den har flyttet seg nok
		Knob knobIds[3] = {Knob::Main, Knob::X, Knob::Y};
		for (int i = 0; i < 3; i++)
		{
			if (knobRef[i] >= 0)
			{
				int32_t d = KnobVal(knobIds[i]) - knobRef[i];
				if (d < 0) d = -d;
				if (d > kPickupThreshold) knobRef[i] = -1;
			}
		}

		bool changed = false;

		// Skriv en parameter bare når knotten faktisk endrer utgangsverdi (dvs.
		// beveges), ikke hver sample. For standalone er dette identisk (knotten
		// er eneste skriver), men det lar en stillestående knott gi slipp så en
		// USB-verdi står — "sist skrevne verdi vinner".
		if (knobRef[1] < 0)
		{
			int newSteps = 1 + QuantizeKnob(Knob::X, kMaxSteps - 1, 1);
			if (newSteps != knobOut[1]) { ch.steps = newSteps; knobOut[1] = newSteps; changed = true; }
		}
		if (knobRef[0] < 0)
		{
			int newFills = QuantizeKnob(Knob::Main, ch.steps, 0);
			if (newFills != knobOut[0]) { ch.fills = newFills; knobOut[0] = newFills; changed = true; }
		}
		if (knobRef[2] < 0)
		{
			int newRot = QuantizeKnob(Knob::Y, ch.steps - 1, 2);
			if (newRot != knobOut[2]) { ch.rotation = newRot; knobOut[2] = newRot; changed = true; }
		}

		if (changed)
		{
			if (ch.fills > ch.steps) ch.fills = ch.steps;
			if (ch.rotation >= ch.steps) ch.rotation = ch.steps - 1;
			ch.Recalc();
			if (ch.currentStep >= ch.steps) ch.currentStep = 0;
		}
		return changed;
	}

	// Registrerer et tap (flikk ned). To gyldige tap etter hverandre setter
	// intern-klokkas periode (samples mellom steg) og faser den til tapet.
	void RegisterTap()
	{
		uint32_t interval = sampleCounter - lastTap;
		if (tapValid && interval >= kMinStepSamples && interval <= kMaxStepSamples)
		{
			internalPeriod = interval;
			internalCounter = 0;
		}
		lastTap = sampleCounter;
		tapValid = true;
		tapFlash = kTrigSamples;
	}
};

// ===========================================================================
// Core 1 — USB CDC (rå TinyUSB)
// ===========================================================================

// Linjebuffer for innkommende kommandoer
static char     g_lineBuf[128];
static uint32_t g_lineLen = 0;
static bool     g_dropLine = false;   // true når inneværende linje er for lang

// Sist sendte snapshot-sekvens (for å unngå å sende samme tilstand to ganger)
static uint32_t g_lastSentSeq = 0;

static void UpperStr(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

// Streng -> int. Setter ok=false ved tom streng eller ugyldige tegn.
static int ParseInt(const char *s, bool &ok)
{
	ok = false;
	if (!s || !*s) return 0;
	bool neg = false;
	if (*s == '-') { neg = true; s++; if (!*s) return 0; }
	int val = 0;
	for (; *s; s++)
	{
		if (*s < '0' || *s > '9') return 0;   // ugyldig tegn -> ok=false
		val = val * 10 + (*s - '0');
		if (val > 100000) return 0;           // overflow-vern
	}
	ok = true;
	return neg ? -val : val;
}

static void SendState(bool force)
{
	if (!tud_cdc_connected()) return;

	shared::Snapshot s;
	uint32_t seq;
	if (!shared::ReadSnapshot(s, seq)) return;    // klarte ikke lese konsistent nå; prøv igjen senere
	if (!force && seq == g_lastSentSeq) return;   // ingenting nytt

	char buf[256];
	int len = snprintf(buf, sizeof(buf),
		"{\"fw\":\"%s\","
		"\"a\":{\"steps\":%d,\"fills\":%d,\"rot\":%d,\"pattern\":%u,\"step\":%d},"
		"\"b\":{\"steps\":%d,\"fills\":%d,\"rot\":%d,\"pattern\":%u,\"step\":%d}}\n",
		EUCLID_VERSION,
		s.ch[shared::CH_A].steps, s.ch[shared::CH_A].fills, s.ch[shared::CH_A].rot,
		(unsigned)s.ch[shared::CH_A].pattern, s.ch[shared::CH_A].step,
		s.ch[shared::CH_B].steps, s.ch[shared::CH_B].fills, s.ch[shared::CH_B].rot,
		(unsigned)s.ch[shared::CH_B].pattern, s.ch[shared::CH_B].step);

	if (len <= 0 || len >= (int)sizeof(buf)) return;

	// Skriv bare hvis hele linja får plass i TX-FIFO — da sender vi aldri en
	// avkuttet JSON-linje. Er det ikke plass ennå, lar vi g_lastSentSeq stå og
	// prøver igjen neste runde (når FIFO-en har tømt seg).
	if (tud_cdc_write_available() < (uint32_t)len) return;

	tud_cdc_write(buf, (uint32_t)len);
	tud_cdc_write_flush();
	g_lastSentSeq = seq;
}

// Tolker én komplett linje. Ugyldige linjer ignoreres stille.
static void HandleLine(char *line)
{
	char *tok[4];
	int n = 0;
	char *p = line;
	while (*p && n < 4)
	{
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		tok[n++] = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		if (*p) *p++ = 0;
	}
	if (n == 0) return;

	UpperStr(tok[0]);

	if (strcmp(tok[0], "GET") == 0)
	{
		SendState(true);
		return;
	}
	if (strcmp(tok[0], "RESET") == 0)
	{
		shared::reqReset = 1;
		return;
	}
	if (strcmp(tok[0], "SET") == 0 && n == 4)
	{
		UpperStr(tok[1]);
		UpperStr(tok[2]);

		int c;
		if      (strcmp(tok[1], "A") == 0) c = shared::CH_A;
		else if (strcmp(tok[1], "B") == 0) c = shared::CH_B;
		else return;

		bool ok = false;
		int v = ParseInt(tok[3], ok);
		if (!ok) return;

		// Valider absolutte grenser her; core0 klemmer i tillegg fills/rot mot
		// gjeldende steps (fills 0..steps, rot 0..steps-1).
		if (strcmp(tok[2], "STEPS") == 0)
		{
			if (v < 1 || v > 16) return;
			shared::reqSteps[c].val = (int16_t)v;
			mem_barrier();
			shared::reqSteps[c].dirty = 1;
		}
		else if (strcmp(tok[2], "FILLS") == 0)
		{
			if (v < 0 || v > 16) return;
			shared::reqFills[c].val = (int16_t)v;
			mem_barrier();
			shared::reqFills[c].dirty = 1;
		}
		else if (strcmp(tok[2], "ROT") == 0)
		{
			if (v < 0 || v > 15) return;
			shared::reqRot[c].val = (int16_t)v;
			mem_barrier();
			shared::reqRot[c].dirty = 1;
		}
		return;
	}
	// Alt annet: ignorer stille.
}

static void PollCdcInput()
{
	if (!tud_cdc_available()) return;

	uint8_t rx[64];
	uint32_t got = tud_cdc_read(rx, sizeof(rx));
	for (uint32_t i = 0; i < got; i++)
	{
		char ch = (char)rx[i];
		if (ch == '\n' || ch == '\r')
		{
			if (!g_dropLine && g_lineLen > 0)
			{
				g_lineBuf[g_lineLen] = 0;
				HandleLine(g_lineBuf);
			}
			g_lineLen = 0;
			g_dropLine = false;
		}
		else if (g_dropLine)
		{
			// forkast til neste linjeskift
		}
		else if (g_lineLen < sizeof(g_lineBuf) - 1)
		{
			g_lineBuf[g_lineLen++] = ch;
		}
		else
		{
			// linjen er for lang -> forkast resten stille
			g_dropLine = true;
			g_lineLen = 0;
		}
	}
}

static void Core1Main()
{
	tud_init(BOARD_TUD_RHPORT);   // initialiser USB-device-stacken på core1 (IRQ på core1)

	uint32_t lastPushMs = 0;
	const uint32_t kMinPushIntervalMs = 34;   // ~30 meldinger/sek maks

	while (true)
	{
		tud_task();          // service USB
		PollCdcInput();      // les og tolk kommandoer

		// Strupet push ved tilstandsendring. Er klokka raskere enn ~30 Hz,
		// slås mellomliggende tilstander sammen (vi sender alltid nyeste).
		if (shared::snapSeq != g_lastSentSeq)
		{
			uint32_t now = to_ms_since_boot(get_absolute_time());
			if ((uint32_t)(now - lastPushMs) >= kMinPushIntervalMs)
			{
				SendState(false);
				lastPushMs = now;
			}
		}
	}
}

int main()
{
	Euclid4Card card;                    // konstrueres på core0 (flash/EEPROM/I2C)
	multicore_launch_core1(Core1Main);   // USB-stacken lever på core1
	card.Run();                          // audio-DSP på core0 (returnerer aldri)
	return 0;
}
