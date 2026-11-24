import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=1b40f8c19ed29802';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=1b40f8c19ed29802';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
