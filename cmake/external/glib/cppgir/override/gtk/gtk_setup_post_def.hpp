#pragma once

// unfortunately gtk includes gtkx includes gdkx includes X11/X*.h
// where the latter brings in a whole slew of (evidently non-namespaced) define
// so try to undefine some of the more nasty ones that might likely conflict
#ifdef DestroyNotify
#undef DestroyNotify
#endif
#ifdef Status
#undef Status
#endif
