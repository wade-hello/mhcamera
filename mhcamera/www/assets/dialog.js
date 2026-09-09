const FOCUSABLE_SELECTOR = [
  "a[href]",
  "button:not([disabled])",
  "input:not([disabled])",
  "select:not([disabled])",
  "textarea:not([disabled])",
  "[tabindex]:not([tabindex='-1'])"
].join(",");

function requireDialog(value) {
  if (!value || typeof value.addEventListener !== "function" || typeof value.showModal !== "function") {
    throw new TypeError("dialog is required");
  }
  return value;
}

function createDialogController(dialog, options = {}) {
  requireDialog(dialog);
  const onClose = typeof options.onClose === "function" ? options.onClose : () => {};
  const getFocusable = typeof options.getFocusable === "function"
    ? options.getFocusable
    : () => Array.from(dialog.querySelectorAll(FOCUSABLE_SELECTOR));
  const initialFocus = options.initialFocus;
  let returnFocus = null;
  let closeReason = "close";

  function isFocusable(element) {
    return Boolean(
      element &&
      !element.disabled &&
      !element.hidden &&
      !element.closest?.("[hidden]") &&
      typeof element.focus === "function"
    );
  }

  function focusableElements() {
    return getFocusable().filter(isFocusable);
  }

  function initialElement() {
    const preferred = typeof initialFocus === "function" ? initialFocus() : initialFocus;
    if (isFocusable(preferred)) return preferred;
    return focusableElements()[0] || null;
  }

  function finishClose() {
    const reason = closeReason;
    closeReason = "close";
    const target = returnFocus;
    returnFocus = null;
    if (target && typeof target.focus === "function") target.focus();
    onClose(reason);
  }

  function close(reason = "close") {
    if (!dialog.open) return false;
    closeReason = reason;
    dialog.close();
    return true;
  }

  function open(trigger) {
    if (dialog.open) return false;
    returnFocus = trigger && typeof trigger.focus === "function" ? trigger : null;
    closeReason = "close";
    dialog.showModal();
    queueMicrotask(() => {
      if (dialog.open) initialElement()?.focus();
    });
    return true;
  }

  function handleKeydown(event) {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      close("cancel");
      return;
    }
    if (event.key !== "Tab") return;

    const focusable = focusableElements();
    if (!focusable.length) {
      event.preventDefault();
      return;
    }
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    const active = dialog.ownerDocument?.activeElement;
    if (event.shiftKey && (active === first || !focusable.includes(active))) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && (active === last || !focusable.includes(active))) {
      event.preventDefault();
      first.focus();
    }
  }

  dialog.addEventListener("keydown", handleKeydown);
  dialog.addEventListener("cancel", (event) => {
    event.preventDefault();
    event.stopPropagation();
    close("cancel");
  });
  dialog.addEventListener("close", finishClose);

  return Object.freeze({ open, close });
}

export { createDialogController };
