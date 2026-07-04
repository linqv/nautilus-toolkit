#pragma once

/**
 * Embedded CSS stylesheet for nautilus-toolkit UI polish.
 *
 * Design principles:
 *   - Respect GNOME HIG and Libadwaita theming (light + dark)
 *   - Use @define-color sparingly; prefer Libadwaita CSS variables
 *   - Subtle enhancements only — no jarring overrides
 */

static const char *const NTK_STYLESHEET = "\n"

/* ── Progress bar ── */
"progressbar.ntk-progress trough {\n"
"  min-height: 10px;\n"
"  border-radius: 6px;\n"
"}\n"
"progressbar.ntk-progress progress {\n"
"  min-height: 10px;\n"
"  border-radius: 6px;\n"
"}\n"

/* ── Status icons on done page ── */
".ntk-status-success {\n"
"  color: @success_color;\n"
"}\n"
".ntk-status-warning {\n"
"  color: @warning_color;\n"
"}\n"
".ntk-status-error {\n"
"  color: @error_color;\n"
"}\n"

/* ── Done page status banner ── */
".ntk-done-banner {\n"
"  padding: 16px;\n"
"  border-radius: 12px;\n"
"}\n"
".ntk-done-banner.ntk-status-success {\n"
"  background: alpha(@success_color, 0.08);\n"
"  color: @success_color;\n"
"}\n"
".ntk-done-banner.ntk-status-warning {\n"
"  background: alpha(@warning_color, 0.08);\n"
"  color: @warning_color;\n"
"}\n"
".ntk-done-banner.ntk-status-error {\n"
"  background: alpha(@error_color, 0.08);\n"
"  color: @error_color;\n"
"}\n"
/* ── Done page status label inherits banner color ── */
".ntk-done-banner label {\n"
"  color: inherit;\n"
"}\n"

/* ── Progress page center layout ── */
".ntk-progress-page {\n"
"  margin: 32px 24px;\n"
"}\n"

/* ── Progress spinner icon ── */
".ntk-progress-icon {\n"
"  opacity: 0.6;\n"
"}\n"

/* ── Result row icons ── */
".ntk-result-ok {\n"
"  color: @success_color;\n"
"}\n"
".ntk-result-fail {\n"
"  color: @error_color;\n"
"}\n"

/* ── Subtle hover lift on action buttons ── */
"button.ntk-action:hover {\n"
"  box-shadow: 0 1px 3px alpha(black, 0.12);\n"
"}\n"

/* ── Cancel button on progress page ── */
"button.ntk-cancel-btn {\n"
"  margin-top: 12px;\n"
"}\n"
;
