using System.Diagnostics;

namespace MacroPadDeck;

/// Turns a stream of mono samples into what the face needs from music:
/// loudness, brightness, tempo and beat phase. Pure DSP — no audio API — so
/// the platform wrappers only feed it, and it can be tested with synthetic
/// audio without playing anything out loud.
///
/// Pipeline, per 1024-sample hop (~21 ms at 48 kHz):
///   log-magnitude spectrum → spectral flux (onset strength) into an 8 s ring.
/// Once a second:
///   detrended flux → autocorrelation over 60-200 BPM, weighted toward ~120
///   to dodge octave errors → tempo; a comb over the last beats → phase.
/// No ML: this is the classic onset-autocorrelation tracker, and it is plenty
/// for a face that nods — the pad keeps time itself between corrections.
public sealed class BeatTracker
{
    const int N = 1024;                    // FFT size and hop (no overlap)
    const double EnvSeconds = 8.0;         // tempo window
    const double MinBpm = 60, MaxBpm = 200;
    const double SilenceDb = -60;

    readonly int _sr;
    readonly double _hopMs;
    readonly object _lock = new();

    readonly float[] _buf = new float[N];
    int _fill;
    readonly float[] _win = new float[N];
    readonly float[] _re = new float[N], _im = new float[N];
    readonly float[] _prevMag = new float[N / 2];
    readonly int[] _rev = new int[N];
    readonly float[] _cos = new float[N / 2], _sin = new float[N / 2];
    readonly int _fluxBins, _lowBins;

    // Onset envelope ring + the Stopwatch time (ms) at the end of each hop
    readonly float[] _env, _envLow;       // full-band and kick-band (<~200 Hz) flux
    readonly double[] _hopEndMs;
    int _envPos;
    long _hops;

    float _loud, _bright;
    double _lastHopMs, _lastLoudMs, _lastTempoMs;
    double _bpm, _lastBeatMs;
    int _conf;
    readonly List<double> _recent = new();   // last few raw tempo estimates, for a median

    public BeatTracker(int sampleRate)
    {
        _sr = sampleRate;
        _hopMs = N * 1000.0 / sampleRate;
        int envLen = (int)Math.Ceiling(EnvSeconds * 1000 / _hopMs);
        _env = new float[envLen];
        _envLow = new float[envLen];
        _hopEndMs = new double[envLen];
        _fluxBins = Math.Min(N / 2, (int)(8000.0 * N / sampleRate));   // onsets live below ~8 kHz
        _lowBins  = Math.Max(2, (int)(200.0 * N / sampleRate));         // kick band

        for (int i = 0; i < N; i++)
            _win[i] = (float)(0.5 - 0.5 * Math.Cos(2 * Math.PI * i / (N - 1)));
        int bits = (int)Math.Log2(N);
        for (int i = 0; i < N; i++)
        {
            int r = 0;
            for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
            _rev[i] = r;
        }
        for (int i = 0; i < N / 2; i++)
        {
            _cos[i] = (float)Math.Cos(-2 * Math.PI * i / N);
            _sin[i] = (float)Math.Sin(-2 * Math.PI * i / N);
        }
    }

    public static double NowMs() => Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency;

    public double HopMs => _hopMs;


    /// Feed mono samples. endMs is the Stopwatch time (ms) of the *last*
    /// sample in the span — hop timestamps are back-computed from it, which is
    /// what lets the beat phase be expressed in wall-clock time.
    public void Process(ReadOnlySpan<float> mono, double endMs)
    {
        lock (_lock)
        {
            for (int i = 0; i < mono.Length; i++)
            {
                _buf[_fill++] = mono[i];
                if (_fill == N)
                {
                    double t = endMs - (mono.Length - 1 - i) * 1000.0 / _sr;
                    Hop(t);
                    _fill = 0;
                }
            }
        }
    }

    void Hop(double tEnd)
    {
        double sq = 0;
        for (int i = 0; i < N; i++) sq += _buf[i] * _buf[i];
        double db = 20 * Math.Log10(Math.Sqrt(sq / N) + 1e-9);

        for (int i = 0; i < N; i++) { _re[_rev[i]] = _buf[i] * _win[i]; _im[_rev[i]] = 0; }
        Fft();

        double flux = 0, fluxLow = 0, wsum = 0, msum = 0;
        for (int k = 1; k < N / 2; k++)
        {
            float m = MathF.Sqrt(_re[k] * _re[k] + _im[k] * _im[k]);
            wsum += k * m; msum += m;
            float lm = MathF.Log(1 + 100 * m);          // log compression: onsets, not level
            float d = Math.Max(0, lm - _prevMag[k]);
            if (k < _fluxBins) flux += d;
            if (k <= _lowBins) fluxLow += d;
            _prevMag[k] = lm;
        }
        bool silent = db < SilenceDb;
        _env[_envPos] = silent ? 0 : (float)flux;
        _envLow[_envPos] = silent ? 0 : (float)fluxLow;
        _hopEndMs[_envPos] = tEnd;
        _envPos = (_envPos + 1) % _env.Length;
        _hops++;
        _lastHopMs = tEnd;

        if (!silent)
        {
            _lastLoudMs = tEnd;
            // ~2.5 s time constant: the face should follow the song, not the snare
            float a = (float)(_hopMs / 2500.0);
            float loud = (float)Math.Clamp((db + 50) / 40, 0, 1);            // -50..-10 dBFS
            double centroidHz = msum > 0 ? wsum / msum * _sr / N : 0;
            float bright = (float)Math.Clamp(Math.Log2(Math.Max(centroidHz, 1) / 500.0) / 3.0, 0, 1);
            _loud += (loud - _loud) * a;
            _bright += (bright - _bright) * a;
        }

        if (tEnd - _lastTempoMs >= 1000) { _lastTempoMs = tEnd; Tempo(); }
    }

    void Fft()
    {
        for (int size = 2; size <= N; size <<= 1)
        {
            int half = size >> 1, step = N / size;
            for (int start = 0; start < N; start += size)
                for (int j = 0; j < half; j++)
                {
                    int a = start + j, b = a + half;
                    float wr = _cos[j * step], wi = _sin[j * step];
                    float tr = wr * _re[b] - wi * _im[b];
                    float ti = wr * _im[b] + wi * _re[b];
                    _re[b] = _re[a] - tr; _im[b] = _im[a] - ti;
                    _re[a] += tr;         _im[a] += ti;
                }
        }
    }

    // Linearise a ring (oldest first), detrend against a ~0.25 s local mean,
    // half-wave rectify, then smooth to ~3 hops wide. The smoothing matters:
    // a one-hop-wide onset peak never lands on an integer lag when the period
    // is fractional (120 BPM = 23.4 hops), and the ACF then prefers 2x the
    // period, where the error happens to cancel — a spurious octave drop.
    double[] Onsets(float[] ring, int L, int start)
    {
        var e = new double[L];
        for (int i = 0; i < L; i++) e[i] = ring[(start + i) % ring.Length];
        int w = Math.Max(1, (int)(125 / _hopMs));
        var o = new double[L];
        for (int i = 0; i < L; i++)
        {
            int a = Math.Max(0, i - w), b = Math.Min(L - 1, i + w);
            double m = 0;
            for (int j = a; j <= b; j++) m += e[j];
            o[i] = Math.Max(0, e[i] - m / (b - a + 1));
        }
        for (int pass = 0; pass < 2; pass++)
        {
            var s = new double[L];
            for (int i = 0; i < L; i++)
                s[i] = 0.5 * o[i] + 0.25 * (i > 0 ? o[i - 1] : o[i]) + 0.25 * (i < L - 1 ? o[i + 1] : o[i]);
            o = s;
        }
        return o;
    }

    static double[] Scaled(double[] x, bool byMax)
    {
        double k = 0;
        if (byMax) foreach (var v in x) k = Math.Max(k, v);
        else { foreach (var v in x) k += v * v; k = Math.Sqrt(k / Math.Max(1, x.Length)); }
        var y = new double[x.Length];
        if (k > 1e-12) for (int i = 0; i < x.Length; i++) y[i] = x[i] / k;
        return y;
    }

    void Tempo()
    {
        int L = (int)Math.Min(_hops, _env.Length);
        if (L < 3000 / _hopMs) { _conf = 0; return; }       // need ~3 s before guessing

        int start = (_envPos - L + _env.Length) % _env.Length;
        var times = new double[L];
        for (int i = 0; i < L; i++) times[i] = _hopEndMs[(start + i) % _env.Length];
        double[] full = Onsets(_env, L, start), low = Onsets(_envLow, L, start);

        // Tempo from both bands at equal weight: kicks reinforce the beat level
        // that broadband hats would otherwise split in two.
        double[] fs = Scaled(full, false), ls = Scaled(low, false);
        var o = new double[L];
        for (int i = 0; i < L; i++) o[i] = fs[i] + ls[i];

        double r0 = 0;
        for (int i = 0; i < L; i++) r0 += o[i] * o[i];
        if (r0 <= 1e-9) { _conf = 0; return; }
        r0 /= L;

        int lagMin = (int)Math.Floor(60000 / (MaxBpm * _hopMs));
        int lagMax = (int)Math.Ceiling(60000 / (MinBpm * _hopMs));
        lagMax = Math.Min(lagMax, L / 2);
        var r = new double[2 * lagMax + 2];
        for (int lag = 1; lag <= Math.Min(2 * lagMax + 1, L - 1); lag++)   // from 1: the half-lag term reads below lagMin
        {
            double s = 0;
            for (int i = lag; i < L; i++) s += o[i] * o[i - lag];
            r[lag] = s / (L - lag);
        }

        // Weighted toward ~120 BPM (one-octave sigma) so a genuine double or
        // half never outvotes the tactus people actually nod to.
        int best = -1; double bestScore = 0;
        for (int lag = lagMin; lag <= lagMax; lag++)
        {
            double bpm = 60000 / (lag * _hopMs);
            double oct = Math.Log2(bpm / 120.0);
            double prior = Math.Exp(-0.5 * (oct / 0.9) * (oct / 0.9));
            // Harmonics both ways: a true tactus also shows at 2x its lag (the
            // next bar) and at half (its off-beats). A 3:2 impostor gets the
            // first but not the second — the one error the prior cannot fix.
            double half = lag / 2 >= 1 ? 0.5 * (r[lag / 2] + r[(lag + 1) / 2]) : 0;
            double score = (r[lag] + (2 * lag < r.Length ? 0.5 * r[2 * lag] : 0) + 0.5 * half) * prior;
            if (score > bestScore) { bestScore = score; best = lag; }
        }
        if (best < 0) { _conf = 0; return; }

        // Sub-hop peak by parabolic interpolation — whole hops alone would
        // quantise 120 BPM to ±2.5 BPM at a 21 ms hop.
        double lagF = best;
        if (best > 1 && best + 1 < r.Length)
        {
            double y0 = r[best - 1], y1 = r[best], y2 = r[best + 1];
            double den = y0 - 2 * y1 + y2;
            if (Math.Abs(den) > 1e-12) lagF = best + Math.Clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
        }
        _recent.Add(60000 / (lagF * _hopMs));
        if (_recent.Count > 5) _recent.RemoveAt(0);
        _bpm = _recent.OrderBy(x => x).ElementAt(_recent.Count / 2);
        _conf = (int)Math.Clamp((r[best] / r0 - 0.08) / 0.35 * 100, 0, 100);

        // Phase on the kick band first: broadband flux is dominated by hats,
        // which sit on the off-beat, and locking to them nods exactly half a
        // beat late. The full band stays in at 30 % for kick-less music.
        double[] lm = Scaled(low, true), fm = Scaled(full, true);
        var p = new double[L];
        for (int i = 0; i < L; i++) p[i] = lm[i] + 0.3 * fm[i];

        double P = 60000 / (_bpm * _hopMs);           // period in hops
        int maxPhi = (int)Math.Ceiling(P);
        var sc = new double[maxPhi + 2];
        int bestPhi = 0; double bestSc = -1;
        for (int phi = 0; phi <= maxPhi + 1; phi++)
        {
            double s = 0, wk = 1;
            for (int k = 0; k < 8; k++, wk *= 0.85)
            {
                int idx = L - 1 - phi - (int)Math.Round(k * P);
                if (idx < 0) break;
                s += wk * p[idx];
            }
            sc[phi] = s;
            if (phi <= maxPhi && s > bestSc) { bestSc = s; bestPhi = phi; }
        }
        double phiF = bestPhi;
        if (bestPhi > 0 && bestPhi + 1 < sc.Length)
        {
            double y0 = sc[bestPhi - 1], y1 = sc[bestPhi], y2 = sc[bestPhi + 1];
            double den = y0 - 2 * y1 + y2;
            if (Math.Abs(den) > 1e-12) phiF = bestPhi + Math.Clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
        }
        // Hop end time minus half a hop = hop centre, where its flux "happened"
        _lastBeatMs = times[L - 1] - phiF * _hopMs - _hopMs / 2;
    }

    public AudioFeatures Read(double nowMs)
    {
        lock (_lock)
        {
            bool active = nowMs - _lastHopMs < 1000 && nowMs - _lastLoudMs < 1500;
            if (!active) return AudioFeatures.Silent with { Loudness = _loud, Brightness = _bright };
            return new AudioFeatures(true, _loud, _bright, _conf > 0 ? _bpm : 0, _lastBeatMs, _conf);
        }
    }
}
