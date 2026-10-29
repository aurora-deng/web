import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=4ec58ab3c1e38adb';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=4ec58ab3c1e38adb';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
