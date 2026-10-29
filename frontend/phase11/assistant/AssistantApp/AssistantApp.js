import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
