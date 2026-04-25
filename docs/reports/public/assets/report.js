function copyText(text) {
  if (!navigator.clipboard || !window.isSecureContext) {
    return Promise.reject(new Error('Clipboard API is unavailable'));
  }
  return navigator.clipboard.writeText(text);
}

document.addEventListener('click', event => {
  const link = event.target.closest('[data-copy-url]');

  if (!link) return;
  event.preventDefault();

  const url = link.getAttribute('data-copy-url');
  if (!url) return;

  copyText(url)
    .then(() => {
      const original = link.textContent;
      link.textContent = 'Copied URL to clipboard.';
      window.setTimeout(() => {
        link.textContent = original;
      }, 1200);
    })
    .catch(() => (window.location.href = url));
});
