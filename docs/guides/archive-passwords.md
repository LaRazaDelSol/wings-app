# Archive passwords and "choose what to keep"

Pegasus holds an archive (ZIP, RAR, 7z) at *waiting for password* until a password arrives. This can be a download or a Files-tab extraction.

## The auto-try

Before Wings asks you, it sends six well-known public passwords, one at a time, each at most once per job, in a fixed order. The line under the job reads "Trying known passwords (n/6)". If one works, the job simply continues.

How Pegasus reports a wrong password has not been verified on a live console. Wings treats a changed detail text, a bounce through *extracting*, or eight seconds of nothing as "did not work". If the auto-try misbehaves, enter the password yourself; the job is waiting for you either way.

## The prompt

When none worked, the job shows **Archive password required** and "Press Cross". Cross opens the on-screen keyboard (the same keys as search; Triangle shows or hides the text). A note under the field says how many known passwords were tried.

Wings has no other way to find a password; it is usually on the release page or with its source.

## Choose what to keep (the waiting-choice dialog)

When an archive holds **more than one package**, Pegasus cannot guess which one you want and the job stops at *waiting choice*. The row says "Choose which file to keep" and "Press Cross". Cross opens the job's actions; **Choose what to keep** lists the packages found in the archive. Pick one and Wings sends your choice to Pegasus, which carries on with that one.

Circle leaves the job waiting. You can come back to it from Downloads.
