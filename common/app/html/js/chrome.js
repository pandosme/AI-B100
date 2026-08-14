/*
 * Shared page chrome.
 *
 * The nav bar used to be copied as markup into every HTML page. That made
 * otherwise-identical platform pages (LoRA Bridge, LoRA Downlink, GPS) differ
 * between apps for no reason beyond their use-case links, so they could not be
 * shared and drifted apart. The markup had also drifted within each app, into
 * three different indentation styles.
 *
 * Each app declares its own page list in js/pages.js as NAV_PAGES, an array of
 * { href, label }. This renders it into <nav class="nav-bar" id="nav-bar">,
 * marks the current page active, and appends the LoRaWAN status dot that the
 * pages update through #lora-nav-dot.
 *
 * Loaded from <head> after jQuery and before the pages' own scripts, so the
 * ready handler here runs first and the nav exists before page code touches it.
 */
$(document).ready(function () {
	var host = $('#nav-bar');
	if (!host.length) return;

	if (typeof NAV_PAGES === 'undefined') {
		console.error('chrome.js: NAV_PAGES is not defined; js/pages.js must load first');
		return;
	}

	var path = window.location.pathname;
	var current = path.substring(path.lastIndexOf('/') + 1) || 'index.html';

	var markup = '';
	for (var i = 0; i < NAV_PAGES.length; i++) {
		var page = NAV_PAGES[i];
		markup += '<a href="' + page.href + '"' +
			(page.href === current ? ' class="active"' : '') + '>' +
			page.label + '</a>';
	}
	markup += '<span id="lora-nav-dot" class="lora-nav-dot" title="LoRaWAN status">&#9679; LoRa</span>';

	host.html(markup);
});
