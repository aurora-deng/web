export async function readSeries(api,keys,bounds,signal,batch=false) {
  const samples=new Map();
  if(batch) {
    for(let offset=0;offset<keys.length&&!signal.aborted;offset+=24) {
      const group=keys.slice(offset,offset+24);
      try {
        const result=await api("series/batch",{...bounds,panels:group.join(",")},signal);
        for(const key of group) samples.set(key,result.panels[key]??null);
      } catch(error) {
        if(!signal.aborted) for(const key of group) samples.set(key,failure(error));
      }
    }
  } else {
    for(let offset=0;offset<keys.length&&!signal.aborted;offset+=5) {
      await Promise.all(keys.slice(offset,offset+5).map(async key=>{
        try {samples.set(key,await api("series",{...bounds,panel:key},signal));}
        catch(error) {if(!signal.aborted) samples.set(key,failure(error));}
      }));
    }
  }
  return samples;
}
export const queryFailed=report=>report==null||report.status==="unavailable";

export function emptyState(reports) {
  const failed=reports.filter(queryFailed);
  if(failed.length) return {
    message:failed.length===reports.length?"数据源不可用":"部分指标查询失败",
    guide:failed.find(report=>report?.error)?.error || "请刷新重试，并检查数据源连接。",
  };
  return reports.find(report=>report?.diagnostic)?.diagnostic || {
    message:"这个时间段暂无有效样本",
    guide:"核对实例和时间范围，调用对应业务后等待采集。",
  };
}

function failure(error) {
  return {status:"unavailable",error:error.message||"查询失败",series:[],step:0};
}
