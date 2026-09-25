#include "phonemes.hpp"

#include <unordered_map>

namespace nvs {

namespace {

constexpr float kNasalPole = 270.0f;

// Formant settings for an adult male reference voice. Voice presets scale
// these (a formant scale of ~1.17 gives an adult female tract, ~1.3 a child).
Params base() {
  Params p{};
  p[F1] = 500; p[F2] = 1500; p[F3] = 2690; p[F4] = 3700; p[F5] = 4300;
  p[B1] = 60;  p[B2] = 90;   p[B3] = 150;  p[B4] = 60;   p[B5] = 100;
  p[FNZ] = kNasalPole;
  p[N1F] = 2500; p[N1B] = 500; p[N2F] = 4500; p[N2B] = 1000; p[N3F] = 7000; p[N3B] = 2000;
  return p;
}

Params vowel(float f1, float f2, float f3, float b1 = 55, float b2 = 45, float b3 = 45) {
  Params p = base();
  p[F1] = f1; p[F2] = f2; p[F3] = f3;
  p[B1] = b1; p[B2] = b2; p[B3] = b3;
  p[AV] = 1.0f;
  return p;
}

// Articulation (formant locus) for a consonant with a given voicing level.
Params locus(float f1, float f2, float f3, float av) {
  Params p = base();
  p[F1] = f1; p[F2] = f2; p[F3] = f3;
  p[B1] = 90; p[B2] = 120; p[B3] = 200;
  p[AV] = av;
  return p;
}

void noise(Params& p, float af, float ab,
           float f1, float b1, float a1,
           float f2, float b2, float a2,
           float f3, float b3, float a3) {
  p[AF] = af; p[AB] = ab;
  p[N1F] = f1; p[N1B] = b1; p[N1A] = a1;
  p[N2F] = f2; p[N2B] = b2; p[N2A] = a2;
  p[N3F] = f3; p[N3B] = b3; p[N3A] = a3;
}

Phoneme V(const char* s, Params a) {
  return {s, PhonClass::Vowel, true, a};
}

Phoneme D(const char* s, Params a, Params b) {
  Phoneme ph{s, PhonClass::Vowel, true, a};
  ph.diphthong = true;
  ph.offglide = b;
  return ph;
}

// Stop consonant: silent (or voice-bar) closure plus a release burst.
Phoneme stop(const char* s, bool voiced, Params loc, Params burst, float dur) {
  loc[AV] = voiced ? 0.25f : 0.0f;
  // During closure the tract is sealed: pull F1 down so voice bars are dull.
  loc[F1] = 200;
  Phoneme ph{s, PhonClass::Stop, voiced, loc};
  ph.burst = burst;
  ph.duration = dur;
  return ph;
}

Phoneme fric(const char* s, bool voiced, Params p, float dur) {
  Phoneme ph{s, PhonClass::Fricative, voiced, p};
  ph.duration = dur;
  return ph;
}

std::vector<Phoneme> buildTable() {
  std::vector<Phoneme> t;

  // --- Vowels ------------------------------------------------------------------
  // F1/F2 measured from Harmony Assistant's Virtual Singer (std voice) singing
  // h_d words (tools/vowel_formants.py), F1 corrected for LPC bias at the test
  // pitch. Like Harmony, F3 sits near 2690 Hz and F4 near 3700 Hz for every
  // vowel except ER, with narrow bandwidths so the upper harmonics ring clearly.
  t.push_back(V("IY", vowel(300, 2180, 2690)));
  t.push_back(V("IH", vowel(400, 1770, 2690)));
  t.push_back(V("EH", vowel(600, 1745, 2690)));
  t.push_back(V("AE", vowel(580, 1550, 2690)));
  t.push_back(V("AA", vowel(550, 1105, 2690)));
  t.push_back(V("AO", vowel(480, 1045, 2690)));
  t.push_back(V("UH", vowel(440, 1085, 2600)));
  t.push_back(V("UW", vowel(330, 1325, 2450)));
  t.push_back(V("AH", vowel(540, 1305, 2660)));
  t.push_back(V("AX", vowel(500, 1400, 2690)));  // schwa (AH0)
  t.push_back(V("ER", vowel(410, 1100, 1460)));

  // --- Diphthongs: sung mostly on the first target, gliding at the end -------
  t.push_back(D("EY", vowel(480, 1750, 2690), vowel(400, 1940, 2690)));
  t.push_back(D("AY", vowel(600, 1250, 2690), vowel(380, 1890, 2690)));
  t.push_back(D("AW", vowel(640, 1310, 2690), vowel(400, 1085, 2690)));
  t.push_back(D("OW", vowel(435, 1220, 2690), vowel(420, 1120, 2690)));
  t.push_back(D("OY", vowel(450, 1050, 2690), vowel(380, 1850, 2690)));

  // --- Stops -------------------------------------------------------------------
  Params labial = locus(250, 850, 2100, 0);
  Params alveolar = locus(250, 1700, 2600, 0);
  Params velar = locus(250, 1900, 2300, 0);

  Params pB = base(); noise(pB, 0.45f, 0.55f, 900, 900, 0.5f, 2500, 2000, 0.2f, 5000, 3000, 0.1f);
  Params tB = base(); noise(tB, 0.85f, 0.05f, 3800, 1200, 0.5f, 5500, 1500, 1.0f, 7500, 2500, 0.5f);
  Params kB = base(); noise(kB, 0.80f, 0.05f, 2100, 500, 1.0f, 3200, 800, 0.5f, 5000, 2000, 0.1f);

  t.push_back(stop("P", false, labial, pB, 0.065f));
  t.push_back(stop("B", true, labial, pB, 0.050f));
  t.push_back(stop("T", false, alveolar, tB, 0.060f));
  t.push_back(stop("D", true, alveolar, tB, 0.045f));
  t.push_back(stop("K", false, velar, kB, 0.070f));
  t.push_back(stop("G", true, velar, kB, 0.050f));

  // --- Fricatives -------------------------------------------------------------
  Params s = locus(300, 1600, 2600, 0);
  noise(s, 0.55f, 0.0f, 4200, 900, 0.3f, 5800, 1400, 1.0f, 7800, 2000, 0.25f);
  Params z = s; z[AV] = 0.35f; z[AF] = 0.38f;

  Params sh = locus(300, 1850, 2450, 0);
  noise(sh, 0.65f, 0.0f, 2600, 500, 1.0f, 3700, 900, 0.6f, 6000, 2000, 0.25f);
  Params zh = sh; zh[AV] = 0.35f; zh[AF] = 0.42f;

  Params f = locus(300, 1000, 2200, 0);
  noise(f, 0.16f, 0.30f, 1500, 1500, 0.2f, 4000, 2500, 0.3f, 7500, 3000, 0.5f);
  Params v = f; v[AV] = 0.50f; v[AF] = 0.08f; v[AB] = 0.12f;

  Params th = locus(300, 1450, 2550, 0);
  noise(th, 0.16f, 0.22f, 2500, 2000, 0.2f, 5000, 3000, 0.4f, 7500, 3000, 0.5f);
  Params dh = th; dh[AV] = 0.55f; dh[AF] = 0.05f; dh[AB] = 0.08f;

  t.push_back(fric("S", false, s, 0.065f));
  t.push_back(fric("Z", true, z, 0.060f));
  t.push_back(fric("SH", false, sh, 0.080f));
  t.push_back(fric("ZH", true, zh, 0.065f));
  t.push_back(fric("F", false, f, 0.095f));
  t.push_back(fric("V", true, v, 0.065f));
  t.push_back(fric("TH", false, th, 0.090f));
  t.push_back(fric("DH", true, dh, 0.045f));

  // --- Affricates: alveolar closure released into a post-alveolar fricative ---
  {
    Phoneme ch = stop("CH", false, locus(250, 1850, 2450, 0), sh, 0.120f);
    ch.cls = PhonClass::Affricate;
    t.push_back(ch);
    Phoneme jh = stop("JH", true, locus(250, 1850, 2450, 0), zh, 0.095f);
    jh.cls = PhonClass::Affricate;
    t.push_back(jh);
  }

  // --- Nasals: low murmur plus an antiresonance that sets the place ----------
  auto nasal = [&](const char* sym, float f2, float zero) {
    Params p = locus(270, f2, 2400, 0.55f);
    p[B1] = 100; p[B2] = 300; p[B3] = 300;
    p[FNZ] = zero;
    Phoneme ph{sym, PhonClass::Nasal, true, p};
    ph.duration = 0.080f;
    return ph;
  };
  t.push_back(nasal("M", 1100, 1000));
  t.push_back(nasal("N", 1600, 1500));
  t.push_back(nasal("NG", 2000, 2400));

  // --- Liquids and glides -------------------------------------------------------
  {
    Params l = locus(330, 1050, 2750, 0.72f);
    l[B1] = 60; l[B2] = 150; l[B3] = 280;
    Phoneme ph{"L", PhonClass::Liquid, true, l};
    ph.duration = 0.075f;
    t.push_back(ph);

    Params r = locus(330, 1100, 1450, 0.78f);
    r[B1] = 70; r[B2] = 100; r[B3] = 120;
    Phoneme rr{"R", PhonClass::Liquid, true, r};
    rr.duration = 0.075f;
    t.push_back(rr);

    Params w = locus(290, 650, 2150, 0.72f);
    w[B1] = 50; w[B2] = 80; w[B3] = 150;
    Phoneme ww{"W", PhonClass::Glide, true, w};
    ww.duration = 0.070f;
    t.push_back(ww);

    Params y = locus(260, 2100, 3000, 0.72f);
    y[B1] = 40; y[B2] = 150; y[B3] = 300;
    Phoneme yy{"Y", PhonClass::Glide, true, y};
    yy.duration = 0.060f;
    t.push_back(yy);
  }

  // --- HH: breath through whatever vowel comes next ---------------------------
  {
    Params h = base();
    h[AH] = 0.85f;
    Phoneme ph{"HH", PhonClass::Aspirate, false, h};
    ph.duration = 0.075f;
    t.push_back(ph);
  }

  return t;
}

}  // namespace

bool isAmplitudeParam(int i) {
  return i == AV || i == AH || i == AF || i == AB || i == N1A || i == N2A || i == N3A;
}

const std::vector<Phoneme>& allPhonemes() {
  static const std::vector<Phoneme> table = buildTable();
  return table;
}

const Phoneme* findPhoneme(const std::string& symbol) {
  static const std::unordered_map<std::string, const Phoneme*> index = [] {
    std::unordered_map<std::string, const Phoneme*> m;
    for (const auto& p : allPhonemes()) m[p.symbol] = &p;
    return m;
  }();
  auto it = index.find(symbol);
  return it == index.end() ? nullptr : it->second;
}

Params silenceLike(const Params& p) {
  Params s = p;
  s[AV] = 0; s[AH] = 0; s[AF] = 0; s[AB] = 0;
  s[FNZ] = kNasalPole;
  return s;
}

}  // namespace nvs
