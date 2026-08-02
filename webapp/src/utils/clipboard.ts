// copyWithFallback copies text to the clipboard, preferring the async
// Clipboard API and falling back to a hidden-textarea execCommand('copy') for
// the contexts where the async API is missing or denied (insecure origin, some
// embedded webviews, older browsers). Returns whether the copy succeeded so the
// caller can flash a confirmation or an error.
export async function copyWithFallback(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // Fall back to the legacy copy path below.
  }

  try {
    const textarea = document.createElement('textarea');
    textarea.value = text;
    textarea.setAttribute('readonly', '');
    textarea.style.position = 'fixed';
    textarea.style.opacity = '0';
    textarea.style.left = '-9999px';
    document.body.appendChild(textarea);
    textarea.select();

    const copied = typeof document.execCommand === 'function' && document.execCommand('copy');
    document.body.removeChild(textarea);
    return copied;
  } catch {
    return false;
  }
}
