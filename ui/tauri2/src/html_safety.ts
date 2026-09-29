const HOST_CLASSES: Readonly<Record<string, string>> = {
  python: "python",
  blender: "blender",
  exe: "exe",
};

/** Escapes untrusted script metadata before inserting it into an HTML text or attribute value. */
export function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  })[character] ?? character);
}

/** Renders a host badge using a whitelist for its CSS class and escaped display text. */
export function renderHostTag(host: string, glyph: string, label: string): string {
  const safeClass = HOST_CLASSES[host] ?? "unknown";
  return `<span class="host-tag ${safeClass}">${escapeHtml(glyph)} ${escapeHtml(label)}</span>`;
}
