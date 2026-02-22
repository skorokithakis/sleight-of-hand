TODO items (remove after confirming correct implementation with the user):

* ~~Consolidate all documentation to p00/t00 terminology.~~ Done: README.md "Tick
  modes" intro rewritten to use p00/t00 notation (task 003).
* ~~Consolidate all modes to use the same machinery.~~ Done: unified tick table
  introduced in task 001.
* ~~Verify that we keep internal time, to keep working when NTP/the internet is
  down.~~ Already correct: `minute_start_ms` is advanced by `+= 60000` each
  minute independently of NTP, so the clock keeps running when the internet is
  down.
