import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=144f1e6009a3d461';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=144f1e6009a3d461';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
