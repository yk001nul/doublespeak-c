#!/usr/bin/env bash
#
# Generates the static download landing page (index.html) for the public
# downloads repo. Emits HTML to stdout.
#
# Usage: gen_downloads_index.sh <tag> <release.json> <latest_dir>
#   <tag>          release tag, e.g. v1.5
#   <release.json> `gh release view --json name,tagName,publishedAt,body`
#   <latest_dir>   dir holding the newest release's binaries (public/downloads/latest)
#
# Links are relative to the page root (downloads/latest/<file>) so they resolve
# from the public repo's own Pages copies, never from the private release URLs.

set -euo pipefail

TAG="$1"
RELEASE_JSON="$2"
LATEST_DIR="$3"

NAME="$(jq -r '.name // .tagName' "$RELEASE_JSON")"
PUBLISHED_RAW="$(jq -r '.publishedAt // empty' "$RELEASE_JSON")"
BODY="$(jq -r '.body // empty' "$RELEASE_JSON")"

# Friendly date; fall back to the raw ISO string if `date -d` can't parse it.
if [ -n "$PUBLISHED_RAW" ] && DATE_FMT="$(date -u -d "$PUBLISHED_RAW" +"%B %-d, %Y" 2>/dev/null)"; then
  PUBLISHED="$DATE_FMT"
else
  PUBLISHED="${PUBLISHED_RAW%%T*}"
fi

html_escape() {
  sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

# Map an asset filename to: icon, platform label, "what's inside" note.
platform_meta() {
  case "$1" in
    *[Ww]indows*|*win*) printf '%s\t%s\t%s' "🪟" "Windows (x64)"  "Contains .exe, .dll and .lib" ;;
    *[Mm]ac*|*darwin*)  printf '%s\t%s\t%s' "🍎" "macOS"          "Contains the .dylib shared library" ;;
    *[Ll]inux*)         printf '%s\t%s\t%s' "🐧" "Linux (x64)"    "Contains the .so shared library and executable" ;;
    *)                  printf '%s\t%s\t%s' "📦" "$1"             "Release asset" ;;
  esac
}

# Emit one card per binary in the latest dir (sorted for stable output).
cards=""
while IFS= read -r f; do
  [ -n "$f" ] || continue
  base="$(basename "$f")"
  bytes="$(stat -c%s "$f")"
  size="$(numfmt --to=iec-i --suffix=B --format='%.1f' "$bytes" 2>/dev/null || echo "${bytes} B")"
  IFS=$'\t' read -r icon label note <<<"$(platform_meta "$base")"
  cards+=$(cat <<CARD
      <a class="card" href="downloads/latest/${base}" download>
        <span class="icon">${icon}</span>
        <span class="platform">${label}</span>
        <span class="file">${base}</span>
        <span class="note">${note}</span>
        <span class="dl">Download &middot; ${size}</span>
      </a>
CARD
)
  cards+=$'\n'
done < <(find "$LATEST_DIR" -maxdepth 1 -type f | sort)

NOTES_HTML=""
if [ -n "$BODY" ]; then
  NOTES_HTML="$(cat <<NOTES
    <details class="notes">
      <summary>Release notes — ${TAG}</summary>
      <pre>$(printf '%s' "$BODY" | html_escape)</pre>
    </details>
NOTES
)"
fi

cat <<HTML
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>doublespeak-c — downloads</title>
<style>
  :root {
    --bg: #f6f8fa; --fg: #1f2328; --muted: #656d76; --card: #ffffff;
    --border: #d0d7de; --accent: #0969da; --accent-fg: #ffffff;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #0d1117; --fg: #e6edf3; --muted: #8b949e; --card: #161b22;
      --border: #30363d; --accent: #2f81f7; --accent-fg: #ffffff;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--bg); color: var(--fg);
    font: 16px/1.6 -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif;
  }
  .wrap { max-width: 860px; margin: 0 auto; padding: 48px 20px 80px; }
  header { border-bottom: 1px solid var(--border); padding-bottom: 24px; margin-bottom: 32px; }
  h1 { margin: 0 0 6px; font-size: 28px; letter-spacing: -0.02em; }
  .sub { color: var(--muted); margin: 0; }
  .badge {
    display: inline-block; background: var(--accent); color: var(--accent-fg);
    font-weight: 600; font-size: 13px; padding: 2px 10px; border-radius: 999px;
    vertical-align: middle; margin-left: 8px;
  }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr)); gap: 16px; }
  .card {
    display: flex; flex-direction: column; gap: 4px; text-decoration: none;
    color: inherit; background: var(--card); border: 1px solid var(--border);
    border-radius: 12px; padding: 20px; transition: border-color .15s, transform .15s;
  }
  .card:hover { border-color: var(--accent); transform: translateY(-2px); }
  .icon { font-size: 28px; }
  .platform { font-weight: 600; font-size: 17px; }
  .file { color: var(--muted); font-size: 13px; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
  .note { color: var(--muted); font-size: 13px; }
  .dl { margin-top: 8px; color: var(--accent); font-weight: 600; font-size: 14px; }
  .notes { margin-top: 40px; border: 1px solid var(--border); border-radius: 12px; background: var(--card); }
  .notes summary { cursor: pointer; padding: 14px 18px; font-weight: 600; }
  .notes pre { margin: 0; padding: 0 18px 18px; white-space: pre-wrap; word-wrap: break-word; color: var(--muted); font-size: 14px; }
  footer { margin-top: 48px; color: var(--muted); font-size: 13px; border-top: 1px solid var(--border); padding-top: 20px; }
  code { background: var(--card); border: 1px solid var(--border); border-radius: 6px; padding: 1px 5px; font-size: 13px; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>doublespeak-c <span class="badge">${TAG}</span></h1>
    <p class="sub">Prebuilt binaries of the Meteor steganographic C library &middot; released ${PUBLISHED}</p>
  </header>

  <div class="grid">
${cards}  </div>

  <p style="color:var(--muted); font-size:14px; margin-top:28px;">
    Each archive is a self-contained build for one platform. Every download here
    is the <strong>${TAG}</strong> release; older versions stay available under
    <code>downloads/&lt;tag&gt;/</code>.
  </p>

${NOTES_HTML}

  <footer>
    Auto-generated on each release. The library requires a running
    <code>llama-server</code> for encode/decode.
  </footer>
</div>
</body>
</html>
HTML
