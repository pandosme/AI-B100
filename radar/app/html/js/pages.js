/* Nav bar contents for the Radar app. Rendered by the shared js/chrome.js.
 *
 * counting.html is intentionally absent: the Counting use case is reserved on
 * port 1 but not developed, and must stay hidden until it is.
 */
const NAV_PAGES = [
	{ href: 'index.html',     label: 'Publish' },
	{ href: 'occupancy.html', label: 'Occupancy' },
	{ href: 'alert.html',     label: 'Detection Alert' },
	{ href: 'aoa.html',       label: 'Radar' },
	{ href: 'bridge.html',    label: 'LoRA Bridge' },
	{ href: 'downlink.html',  label: 'LoRA Downlink' },
	{ href: 'gps.html',       label: 'GPS' },
	{ href: 'about.html',     label: 'About' }
];
