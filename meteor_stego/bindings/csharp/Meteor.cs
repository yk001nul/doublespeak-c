using System;
using System.Runtime.InteropServices;
using System.Text;

namespace MeteorStego
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct MeteorConfig
    {
        public IntPtr  KeyRaw;          // 32-byte raw key pointer, or IntPtr.Zero
        public IntPtr  KeyInput;        // arbitrary key material pointer, or IntPtr.Zero
        public UIntPtr KeyInputLen;
        public IntPtr  Salt;            // 32-byte HKDF salt pointer, or IntPtr.Zero
        public UIntPtr SaltLen;
        public int     Beta;
        public int     NumCandidates;
        [MarshalAs(UnmanagedType.LPStr)] public string LlmUrl;
        [MarshalAs(UnmanagedType.LPStr)] public string HyphenDict;
        public int     MaxSteps;
        public int     LlmTimeoutMs;
        // Must stay last, matching MeteorConfig's field order in meteor.h.
        // Without it, meteor_create() reads config->style past the end of
        // this (smaller) struct — an out-of-bounds read. 0 = METEOR_STYLE_NONE
        // (legacy mode, matching this binding's current behavior); this
        // binding doesn't yet expose a way to select the other styles.
        public int     Style;
    }

    public static class MeteorNative
    {
        // Library name resolved by the .NET runtime:
        //   Windows  → meteor.dll
        //   Linux    → libmeteor.so
        //   macOS    → libmeteor.dylib
        private const string Lib = "meteor";

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr meteor_create(ref MeteorConfig cfg);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void meteor_destroy(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr meteor_encode(IntPtr ctx,
            [In] byte[] message, UIntPtr msgLen,
            [MarshalAs(UnmanagedType.LPStr)] string startingContext,
            out int error);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr meteor_decode(IntPtr ctx,
            [MarshalAs(UnmanagedType.LPStr)] string covertext,
            [MarshalAs(UnmanagedType.LPStr)] string startingContext,
            out UIntPtr msgLen, out int error);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr meteor_syllabify_word(IntPtr ctx,
            [MarshalAs(UnmanagedType.LPStr)] string word);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int meteor_llm_health(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void meteor_free(IntPtr ptr);
    }

    /// <summary>
    /// Managed wrapper around the Meteor stego C library.
    /// Compatible with Godot 4's .NET runtime via P/Invoke.
    /// Place meteor.dll / libmeteor.so in the project root or addons/meteor/.
    /// </summary>
    public sealed class Meteor : IDisposable
    {
        private IntPtr   _ctx;
        private GCHandle _keyHandle;
        private bool     _disposed;

        /// <param name="keyInput">Key material (passphrase bytes, ECDH output, etc.).</param>
        /// <param name="salt">32-byte random salt shared out-of-band; null = zero salt.</param>
        /// <param name="beta">Bits per Meteor step (2–5, default 3).</param>
        /// <param name="numCandidates">Syllable candidates per LLM call (4–8, default 6).</param>
        /// <param name="llmUrl">URL of the running llama-server.</param>
        /// <param name="hyphenDict">Path to hyph_en_US.dic, or null for heuristic.</param>
        public Meteor(byte[] keyInput,
                      byte[] salt          = null,
                      int    beta          = 3,
                      int    numCandidates = 6,
                      string llmUrl        = "http://127.0.0.1:8080",
                      string hyphenDict    = null)
        {
            _keyHandle = GCHandle.Alloc(keyInput, GCHandleType.Pinned);

            GCHandle saltHandle = default;
            if (salt != null)
                saltHandle = GCHandle.Alloc(salt, GCHandleType.Pinned);

            var cfg = new MeteorConfig
            {
                KeyRaw        = IntPtr.Zero,
                KeyInput      = _keyHandle.AddrOfPinnedObject(),
                KeyInputLen   = (UIntPtr)keyInput.Length,
                Salt          = salt != null ? saltHandle.AddrOfPinnedObject() : IntPtr.Zero,
                SaltLen       = (UIntPtr)(salt?.Length ?? 0),
                Beta          = beta,
                NumCandidates = numCandidates,
                LlmUrl        = llmUrl,
                HyphenDict    = hyphenDict,
                MaxSteps      = 256,
                LlmTimeoutMs  = 30000,
                Style         = 0, // METEOR_STYLE_NONE
            };

            _ctx = MeteorNative.meteor_create(ref cfg);

            if (salt != null) saltHandle.Free();

            if (_ctx == IntPtr.Zero)
                throw new InvalidOperationException(
                    "meteor_create() failed — check libsodium installation and configuration.");
        }

        /// <summary>Encode message bytes into LLM-generated covertext.</summary>
        public string Encode(byte[] message, string startingContext)
        {
            ThrowIfDisposed();
            IntPtr ptr = MeteorNative.meteor_encode(
                _ctx, message, (UIntPtr)message.Length, startingContext, out int err);
            if (err != 0)
                throw new InvalidOperationException($"meteor_encode error code {err}");
            string result = Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
            MeteorNative.meteor_free(ptr);
            return result;
        }

        /// <summary>Decode covertext back to the original message bytes.</summary>
        public byte[] Decode(string covertext, string startingContext)
        {
            ThrowIfDisposed();
            IntPtr ptr = MeteorNative.meteor_decode(
                _ctx, covertext, startingContext,
                out UIntPtr msgLen, out int err);
            if (err != 0)
                throw new InvalidOperationException($"meteor_decode error code {err}");
            byte[] result = new byte[(int)msgLen];
            Marshal.Copy(ptr, result, 0, result.Length);
            MeteorNative.meteor_free(ptr);
            return result;
        }

        /// <summary>
        /// Syllabify a word. Returns syllables joined by middle-dot (e.g. "re·mark·a·ble").
        /// </summary>
        public string SyllabifyWord(string word)
        {
            ThrowIfDisposed();
            IntPtr ptr = MeteorNative.meteor_syllabify_word(_ctx, word);
            if (ptr == IntPtr.Zero) return word;
            string result = Marshal.PtrToStringAnsi(ptr) ?? word;
            MeteorNative.meteor_free(ptr);
            return result;
        }

        /// <summary>Returns true if the configured LLM server is reachable.</summary>
        public bool IsLlmHealthy()
        {
            ThrowIfDisposed();
            return MeteorNative.meteor_llm_health(_ctx) != 0;
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            if (_ctx != IntPtr.Zero)
            {
                MeteorNative.meteor_destroy(_ctx);
                _ctx = IntPtr.Zero;
            }
            if (_keyHandle.IsAllocated)
                _keyHandle.Free();
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
                throw new ObjectDisposedException(nameof(Meteor));
        }
    }
}
