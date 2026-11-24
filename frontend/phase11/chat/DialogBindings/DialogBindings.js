import { createChatScope } from '../ChatScope/ChatScope.js?v=9fcaa747c305a8cc';
export const dependencies={"resources":[],"commands":[],"types":[],"events":true};
/** Dialog UI subscriptions. All listeners belong to the page lifecycle. */
export function bindDialog(context) {
  const scope=createChatScope(context,dependencies);
  const {listen}=scope;
document.querySelectorAll('[data-close-dialog]').forEach(button => {
  listen(button, 'click', () => button.closest('dialog').close());
});
document.querySelectorAll('dialog').forEach(dialog => {
  listen(dialog, 'click', event => {
    if (event.target === dialog) dialog.close();
  });
});
}
