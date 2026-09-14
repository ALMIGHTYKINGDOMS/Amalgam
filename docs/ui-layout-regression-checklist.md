# Amalgam UI Layout Regression Checklist

Run this checklist at the enforced minimum window size (960x600), the default size, and a wide size before a release build.

- [x] Page content scrolls only when content exceeds the viewport.
- [x] Long lists and logs own their child scrolling without hiding the page footer.
- [x] Modal content can scroll while the close, Cancel, and primary footer controls remain reachable.
- [x] Empty states are centered and keep their action visible.
- [x] Toolbars and primary actions stay inside the content region at the minimum size.
- [x] No horizontal overflow was visible in the 22-route 960x600 matrix.
- [x] Branding art remains visible and consistent on Home, Discover, Servers, Essentials, Bedrock, Settings, Downloads, and Java Manager.
- [x] Verify the demonstrated child-scroll ownership issue at the compact size: the sidebar no longer captures page-wheel input, and a fitting Settings section remains stationary after wheel input.

Evidence: `C:\Users\David\AppData\Local\Temp\amalgam-route-matrix-20260831-fixed-sidebar`.
