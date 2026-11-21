import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=7f8c7f1b31e4435c';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=7f8c7f1b31e4435c';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
