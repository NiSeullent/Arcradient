/* NightScape Dark Mode - Document Attribute & Theme Telemetry Hook */
(function () {
    if (typeof document !== "undefined" && document.documentElement && document.documentElement.setAttribute) {
        document.documentElement.setAttribute("data-waterscape-dark", "1");
    }
    if (typeof console !== "undefined" && console.log) {
        console.log("[NightScape] data-waterscape-dark=\"1\" active");
    }
})();
