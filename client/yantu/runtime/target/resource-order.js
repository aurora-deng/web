(() => {
    const kinds = {'textbook':'教材','workbook':'习题册','past-paper':'历年真题','mock-paper':'模拟试卷','solution':'答案解析','answer-sheet':'答题卡','reference':'参考资料','note':'学习笔记','mistake':'错题','mindmap':'思维导图','audio':'音频资料','video':'视频资料','image':'图片资料','other':'其他资料'};
    const formats = {pdf:'PDF',image:'图片',audio:'音频',video:'视频',document:'文档',sheet:'表格',slides:'演示文稿',mindmap:'导图文件',archive:'压缩包',other:'其他文件',text:'无附件'};
    const kindRank=new Map(Object.keys(kinds).map((key,index)=>[key,index]));
    const collator = new Intl.Collator('zh-CN', {numeric:true,sensitivity:'base'});
    const compare = (a,b) => collator.compare(String(a ?? ''),String(b ?? ''));
    const attachments = record => [...(record.files || []),...(record.fileId && !(record.files || []).some(file=>file.id===record.fileId) ? [{id:record.fileId,name:record.fileName||'附件.pdf',type:'application/pdf'}] : [])];
    function format(file) {
        const mime=String(file.type||'').toLowerCase(),ext=String(file.name||'').split('.').pop().toLowerCase();
        if(mime==='application/pdf'||ext==='pdf')return 'pdf';
        if(mime.startsWith('image/')||/^(png|jpe?g|gif|webp|bmp|heic|tiff?|svg)$/.test(ext))return 'image';
        if(mime.startsWith('audio/')||/^(mp3|wav|m4a|aac|flac|ogg|wma)$/.test(ext))return 'audio';
        if(mime.startsWith('video/')||/^(mp4|webm|mkv|mov|avi|m4v)$/.test(ext))return 'video';
        if(/^(xmind|mm|mmap|opml|km)$/.test(ext))return 'mindmap';
        if(/^(zip|rar|7z|tar|gz)$/.test(ext)||/zip|compressed/.test(mime))return 'archive';
        if(/^(xlsx?|csv|ods)$/.test(ext)||/spreadsheet|excel/.test(mime))return 'sheet';
        if(/^(pptx?|odp)$/.test(ext)||/presentation|powerpoint/.test(mime))return 'slides';
        if(/^(docx?|txt|md|rtf|epub|html?|odt)$/.test(ext)||mime.startsWith('text/')||/word|epub/.test(mime))return 'document';
        return 'other';
    }
    function kind(record,col) {
        if(Object.hasOwn(kinds,record.role))return record.role;
        if(col==='notes')return 'note';
        if(col==='mistakes')return 'mistake';
        if(col==='mindmaps')return 'mindmap';
        // Only classify from short labels and names, never from the full book text.
        const labels=[record.path,...(Array.isArray(record.tags)?record.tags:[])].filter(Boolean).join(' '),name=String(record.title||'');
        for(const [key,pattern] of [['answer-sheet',/答题卡|答题纸/],['solution',/答案解析|习题全解|参考答案|题解/],['past-paper',/历年真题|纯真题|真题试卷/],['mock-paper',/模拟试卷|模拟题|套卷|预测卷/],['textbook',/教材|教科书/],['workbook',/习题册|练习册|题库/],['mindmap',/思维导图|原始导图/]])if(pattern.test(labels))return key;
        for(const [key,pattern] of [['answer-sheet',/答题卡|答题纸/],['solution',/答案解析|习题全解|参考答案|试题解析/],['past-paper',/真题/],['mock-paper',/模拟|套卷|预测卷/]])if(pattern.test(name))return key;
        if(col==='papers')return 'past-paper';
        const types=new Set(attachments(record).map(format));
        if(types.size===1&&['audio','video','image','mindmap'].includes([...types][0]))return [...types][0];
        return 'reference';
    }
    const validYear = value => /^(19|20|21)\d{2}$/.test(String(value)) ? Number(value) : null;
    function years(value) {
        if(value&&typeof value==='object'&&!Array.isArray(value)){
            const from=validYear(value.from),to=validYear(value.to);
            if(from&&to&&to>=from)return Array.from({length:to-from+1},(_,i)=>from+i);
            return [];
        }
        if(Array.isArray(value))return [...new Set(value.flatMap(years))].sort((a,b)=>a-b);
        const text=String(value??'').trim(),range=text.match(/^((?:19|20|21)\d{2})\s*[-~至到–—]\s*((?:19|20|21)\d{2})(?:年|合集)?$/);
        if(range)return years({from:range[1],to:range[2]});
        if(/^(?:(?:19|20|21)\d{2}年?)(?:\s*[,，、;；]\s*(?:19|20|21)\d{2}年?)*$/.test(text))return [...new Set(text.match(/(?:19|20|21)\d{2}/g).map(Number))].sort((a,b)=>a-b);
        return [];
    }
    function yearText(value) {
        const list=years(value);if(!list.length)return '';
        return list.length>1&&list.every((year,index)=>!index||year===list[index-1]+1)?`${list[0]}-${list.at(-1)}`:list.join('、');
    }
    function parseYearInput(value) {
        const text=String(value||'').trim();if(!text)return '';
        const list=years(text);if(!list.length)throw new Error('年份请填写四位年份、年份区间或以逗号分隔的多个年份');
        return list.length===1?list[0]:/^\d{4}\s*[-~至到–—]/.test(text)?{from:list[0],to:list.at(-1)}:list;
    }
    function date(value) {
        const match=String(value||'').match(/^(\d{4})-(\d{2})-(\d{2})(?:$|T|\s)/);
        if(!match)return '';
        const text=match[0].slice(0,10),parsed=new Date(text+'T00:00:00Z');
        return Number.isFinite(+parsed)&&parsed.toISOString().slice(0,10)===text?text:'';
    }
    const timestamp = value => {const parsed=Date.parse(value||'');return Number.isFinite(parsed)?parsed:0;};
    function resourceYears(record) {
        const explicit=years(record.year??record.applicableYear??record.publicationYear);
        if(explicit.length)return explicit;
        // Imported titles have a dedicated year segment. Unverified versions stay unknown.
        return [...new Set(String(record.title||'').split(/[|｜]/).flatMap(part=>years(part.trim().replace(/年版$|版$/,''))))].sort((a,b)=>a-b);
    }
    function describe(record,col,usage=new Map()) {
        const recordFiles=attachments(record),list=resourceYears(record),recordDate=date(record.date)||date(record.createdAt)||date(record.importedAt);
        const type=kind(record,col),fileTypes=[...new Set(recordFiles.map(format))];
        return {kind:type,formats:fileTypes.length?fileTypes:['text'],years:list,year:list.at(-1)||0,yearLabel:yearText(list),date:recordDate,month:recordDate.slice(0,7),used:Math.max(timestamp(record.lastUsedAt),usage.get(record.id)||0)};
    }
    function group(record,meta,by) {
        if(by==='kind')return {key:meta.kind,label:kinds[meta.kind]};
        if(by==='subject')return {key:String(record.subject||String(record.path||'').split(/\s*(?:>|→|\/|\\)\s*/)[0]||'未分类'),label:String(record.subject||String(record.path||'').split(/\s*(?:>|→|\/|\\)\s*/)[0]||'未分类')};
        if(by==='year')return meta.years.length>1?{key:'$collection',label:'跨年合集'}:{key:String(meta.year||''),label:meta.year?meta.year+' 年':'年份待核验'};
        if(by==='month')return {key:meta.month,label:meta.month||'日期待补充'};
        if(by==='format'){const key=meta.formats.length>1?'mixed':meta.formats[0];return {key,label:key==='mixed'?'多种附件':formats[key]};}
        return {key:'',label:''};
    }
    function groupCompare(a,b,by,sort) {
        if(a.key===b.key)return 0;
        if(by==='year'||by==='month'){
            const special=value=>value==='$collection'?1:value?0:2,rank=special(a.key)-special(b.key);
            if(rank)return rank;
            return sort==='year-asc'||sort==='oldest'?compare(a.key,b.key):compare(b.key,a.key);
        }
        if(by==='kind')return (kindRank.get(a.key)??100)-(kindRank.get(b.key)??100);
        return compare(a.label,b.label);
    }
    function recordCompare(a,b,sort,metadata) {
        const x=metadata.get(a),y=metadata.get(b);
        const missingLast=(left,right,ascending=false)=>!left&&!right?0:!left?1:!right?-1:typeof left==='number'&&typeof right==='number'?(ascending?left-right:right-left):ascending?compare(left,right):compare(right,left);
        let result=0;
        if(sort==='used')result=missingLast(x.used,y.used);
        else if(sort==='year-desc'||sort==='year-asc')result=missingLast(x.year,y.year,sort==='year-asc')||missingLast(x.years[0],y.years[0],sort==='year-asc');
        else if(sort==='kind')result=groupCompare(group(a,x,'kind'),group(b,y,'kind'),'kind',sort);
        else if(sort!=='title')result=missingLast(x.date,y.date,sort==='oldest');
        return result||compare(a.title,b.title)||compare(a.id,b.id);
    }
    function prepare(records,col,ui,positions=[]) {
        const usage=new Map();for(const position of positions){const time=timestamp(position.updatedAt);if(time>(usage.get(position.sourceId)||0))usage.set(position.sourceId,time);}
        const metadata=new Map(records.map(record=>[record,describe(record,col,usage)])),groups=new Map(records.map(record=>[record,group(record,metadata.get(record),ui.group)]));
        const comparator=(a,b)=>groupCompare(groups.get(a),groups.get(b),ui.group,ui.sort)||recordCompare(a,b,ui.sort,metadata);
        return {metadata,comparator};
    }
    const api={kinds,formats,compare,attachments,format,kind,years,yearText,parseYearInput,date,timestamp,resourceYears,describe,group,prepare};
    if(typeof module!=='undefined'&&module.exports)module.exports=api;
    else window.ResourceOrder=api;
})();
