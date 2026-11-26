import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=0613976363093f95';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=0613976363093f95';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
