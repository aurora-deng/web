/** Explicit controller ports. A module sees only its declared resources/commands.
 * Commands resolve lazily because composition installs cooperating controllers
 * before any user action runs. The shared account/session state is not copied. */
export function createChatScope(context, contract) {
  const pick=(source, names)=>Object.fromEntries(names.map(name=>{
    if(!(name in source))throw new Error(`Missing chat dependency: ${name}`);
    return [name,source[name]];
  }));
  const result=pick(context,contract.resources||[]);
  result.types=Object.freeze(pick(context.types,contract.types||[]));
  result.actions=Object.freeze(Object.fromEntries((contract.commands||[]).map(name=>[
    name,(...args)=>{
      const action=context.actions[name];
      if(typeof action!=='function')throw new Error(`Chat command unavailable: ${name}`);
      return action(...args);
    },
  ])));
  if(contract.events)result.listen=(target,event,handler,options={})=>{
    if(!target)throw new Error(`Missing chat event target: ${event}`);
    target.addEventListener(event,handler,{...options,signal:context.runtime.lifecycle.signal});
  };
  return Object.freeze(result);
}

/** Validate the completed composition, after lazy command providers are installed. */
export function assertChatAssembly(context, contracts) {
  for(const contract of contracts)for(const name of contract.commands||[])
    if(typeof context.actions[name]!=='function')throw new Error(`Missing chat command: ${name}`);
}
