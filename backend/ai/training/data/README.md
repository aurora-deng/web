# 授权训练数据

TrainingDataService 和 JsonlTrainingDataExporter 处理已有授权流程及 JSONL 导出。
输入为经同意的数据选择和授权记录；输出为有界训练样本、摘要与来源信息。
本目录不自动训练、不下载模型；数据访问、导出、批准及审计仍遵循原服务边界。
本次纯路径迁移不修改 SQL、权限或真实数据库。
