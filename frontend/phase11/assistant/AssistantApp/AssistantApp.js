import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=227af12a6c379d08';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=227af12a6c379d08';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
