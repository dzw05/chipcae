# Run monitor

Select `EventFormat::LegacyCaelog1` explicitly for the existing CAELOG1/CRC-32
format. `Ccevlog` is an unsupported placeholder, including CRC32C and segmented
64 MiB logs. No real-kernel replacement-wire compatibility is claimed.

`MainWindow::monitorRun` attaches a directory, format and optional D11 broker/run
ID. The File menu opens an explicitly labelled legacy log in read-only monitoring
mode; the STOP button becomes available when a caller supplies the broker.
The monitor guards the optional broker with a non-owning `QPointer`. If the broker
is destroyed before the dock, STOP becomes a safe no-op without starting its
timeout clock, and broker status queries are skipped. The timer continues polling
files and refreshing the monitor. Broker calls remain on the owning Qt thread;
status-query exceptions are shown as de-duplicated diagnostics.

The tailer persists a byte cursor and sequence watermark. On inode replacement or
observed truncation it restarts at zero, filtering sequences already delivered.
The run must retain monotonic event sequences across reopen/replacement.
Incomplete headers/bodies wait silently. Bad magic, CRC, payloads and unsupported
Hello formats retain the last validated cursor and produce diagnostics.

The desktop adapter validates framing and stages each complete record in a
QTemporaryFile before using the shared EventLogReader decoder. This deliberately
avoids losing a valid prefix when EventLogReader throws on a later payload. It
reads only new records, but incurs one temporary file and a second CRC pass per
record; this is a performance deviation from a direct persistent-reader adapter.
No binary payload decoder is duplicated in desktop.

Both residual payloads feed lossless chart data, grouped by solver, field,
window, and time when present. Every point retains its flags column. Log levels
remain raw signed integers pending owner-approved mappings.

STOP delivery remains Pending after append and after timeout; only a matching
sequence AND request ID yields APPLIED, NOOP or REJECTED. KernelExit and process
exit must both arrive before terminal classification. OutputAvailable consults
manifest.json and never trusts its event path as an available output.

The offscreen test constructs the Qt chart host, then renders its actual chart
in a standalone VTK context. It checks points/flags, OpenGL, nonconstant pixels,
and writes desktop-monitor.png in the build directory. For a display-backed Qt/VTK capture, run the same CTest with
`CHIPCAE_MONITOR_PRESENT=1` under Xvfb (or an X11 session); this additionally checks
the actual widget OpenGL context and writes desktop-monitor-qt.png. Human
interactive navigation remains a separate manual check.
