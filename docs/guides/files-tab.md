# The Files tab

Files shows the console's file system from `/`, through Pegasus' file API. Wings changes nothing here by itself; every operation is a request to Pegasus.

## Move around

- **Cross** opens a folder, **Circle** goes up (or clears the filter or selection first).
- Drive chips and a breadcrumb at the top show where you are.
- **Touchpad click** filters the list.

## Select and act

- **Square** marks a file or folder. Mark several, or use **Select everything here**.
- **Triangle** opens the actions: Move to, Copy to, Rename, Delete, Extract to (archives), Install package (`.pkg`), New folder, Sort, Filter, Refresh, Details.
- Move, copy and extract open **Where to?**, a browser over the real file system: Cross opens a folder, Triangle chooses it, Square picks what to do if the name exists, Circle goes up.
- **Delete** removes files from this console for good, folders with everything inside. It asks first and the focus starts on Cancel.

## Operations

**Options** shows the operations Pegasus is running. Select one: Square cancels it ("Stop it"), Triangle opens its destination. A cancelled large cross-drive move may leave a partial file at the destination; delete it yourself. (Whether it does is not yet verified on a console.)

## Advice

- Try a new operation on a scratch folder first (Triangle, New folder).
- Do not delete in the downloads folder, a game folder or a system location unless you mean it.
- Pegasus may refuse writes to some folders; Wings shows its message.
- Extract and install are carried out by Pegasus. If it fails, see [Troubleshooting](troubleshooting.md#extraction-failed--pegasus-removed-the-files).
