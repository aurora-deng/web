const key = 'nebula:theme';
/** 主题只保存展示偏好；隐私模式下存储不可用也不影响消息。 */
export function applyTheme(value) {
  const theme = value === 'light' ? 'light' : 'dark';
  document.documentElement.dataset.theme = theme;
  document.documentElement.style.colorScheme = theme;
  try { if (localStorage.getItem(key) !== theme) localStorage.setItem(key, theme); } catch {}
  document.querySelectorAll('[data-theme-toggle]').forEach(button => {
    button.textContent = theme === 'dark' ? '☀' : '☾';
    button.setAttribute('aria-label', `切换${theme === 'dark' ? '浅' : '深'}色主题`);
    button.setAttribute('aria-pressed', String(theme === 'light'));
  });
  window.dispatchEvent(new CustomEvent('nebula:theme', { detail: theme }));
}
export function mountTheme(button) {
  // Palette and workspace surfaces are loaded once by DesignSystem/tokens.css.
  // Avoid a late stylesheet that overrides components after their first render.
  let saved; try { saved = localStorage.getItem(key); } catch {}
  button.dataset.themeToggle = '';
  button.addEventListener('click', () => applyTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));
  applyTheme(saved);
  window.addEventListener('storage', event => { if (event.key === key) applyTheme(event.newValue); });
}
