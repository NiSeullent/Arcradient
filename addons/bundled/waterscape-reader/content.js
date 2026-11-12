/* Waterscape Reader View - Reader Mode Marker Hook */
(function () {
    if (typeof document !== "undefined" && document.documentElement && document.documentElement.setAttribute) {
        document.documentElement.setAttribute("data-waterscape-reader", "1");
    }
    if (typeof console !== "undefined" && console.log) {
        console.log("[Waterscape Reader] Reader layout active (max-width: 720px)");
    }
})();
