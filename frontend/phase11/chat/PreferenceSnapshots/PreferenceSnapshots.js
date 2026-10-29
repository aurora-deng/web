/** Versioned per-user preferences: an old HTTP list cannot undo a newer WS/PATCH result. */
export class PreferenceSnapshots {
  constructor(){this.rows=new Map();}
  clear(){this.rows.clear();}
  absorb(id,muted,revision='0'){
    let version;try{version=BigInt(revision||'0');}catch{version=0n;}
    const old=this.rows.get(String(id));
    if(old && (old.version>version || (version>0n && old.version===version)))return old.muted;
    this.rows.set(String(id),{version,muted:Boolean(muted)});return Boolean(muted);
  }
}
