\# 代码签名政策



本项目使用 \[SignPath Foundation](https://signpath.org/) 提供的免费代码签名服务。



\## 签名范围



所有正式发布的 Release 版本（.exe 文件）均会被签名。



\## 角色定义



\- \*\*作者 (Author)\*\*：@yvziyao，负责编写代码和提交 PR。

\- \*\*审阅者 (Reviewer)\*\*：@yvziyao，负责审查代码变更。

\- \*\*审批人 (Approver)\*\*：@yvziyao，负责批准最终发布版本。



\## 签名流程



1\. 代码变更通过 Pull Request 提交

2\. 审阅者审核通过后合并到主分支

3\. 审批人触发 GitHub Actions 构建

4\. 构建产物自动提交至 SignPath 进行签名

5\. 签名后的文件发布到 GitHub Releases



\## 安全要求



\- 所有代码变更必须经过审阅

\- 项目成员必须开启双因素认证 (2FA)

\- 私钥由 SignPath Foundation 安全保管，项目成员无法直接接触

