"""Experiment identity validation and comparison use cases."""
import re
from tools.observability.observer.contracts.events import Invalid
from tools.observability.observer.analysis.projection import compare


class ExperimentQueries:
    def __init__(self,store):
        self.store=store

    def detail(self,ident):
        if not isinstance(ident,str) or not re.fullmatch('[0-9a-f]{64}',ident):
            raise Invalid('实验编号无效')
        return self.store.run(ident)

    def compare(self,baseline,current):
        left,right=self.detail(baseline),self.detail(current)
        return (404,{'error':'实验记录不存在'}) if left is None or right is None else (200,compare(left,right))
