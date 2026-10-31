import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=dc39cdb5489c964d';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=dc39cdb5489c964d';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
