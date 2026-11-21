/* Classic bootstrap also reports version mismatches when an old process serves new HTML. */
(async () => {
  const status = document.getElementById("boot-status");
  try {
    await import("/workspace.js");
  } catch (error) {
    status.replaceChildren();
    const heading = document.createElement("strong");
    heading.textContent = "监控页面资源加载失败";
    const hint = document.createElement("p");
    hint.textContent = "请确认入口对应新版 Observer。重启旧的预览或检查 SSH 隧道，再刷新页面。";
    const retry = document.createElement("button");
    retry.textContent = "重新加载";
    retry.onclick = () => location.reload();
    status.append(heading, hint, retry);
    console.error("Observer startup failed", error);
  }
})();
