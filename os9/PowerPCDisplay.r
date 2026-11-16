/* Resources for the Mac OS 9 receiver: memory, what events it takes, version. */
#include "Processes.r"
#include "MacTypes.r"

resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,              /* keeps answering the Mac while in the background */
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	isHighLevelEventAware,      /* Quit from the Finder / shutdown */
	localAndRemoteHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	24 * 1024 * 1024,           /* preferred: picture + tile cache at millions of colours */
	10 * 1024 * 1024            /* minimum: works with a smaller cache */
};

resource 'vers' (1) {
	0x00, 0x14, release, 0x00, verUS,
	"0.14",
	"0.14, PowerPC Display for Mac OS 9"
};
