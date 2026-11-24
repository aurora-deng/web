import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=9fcaa747c305a8cc';
import { AssistantPanel } from '../../shared/AssistantPanel/AssistantPanel.js?v=9fcaa747c305a8cc';
mountNavigation('assistant');
const panel=new AssistantPanel(document.getElementById('private-friend'),{surface:'friend'});
await panel.start();window.addEventListener('pagehide',()=>panel.dispose());
