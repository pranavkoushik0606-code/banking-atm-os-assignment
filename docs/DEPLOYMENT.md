# Working deployment

- Live URL: https://pranav-banking-atm.onrender.com/
- Health endpoint: https://pranav-banking-atm.onrender.com/health
- Source: https://github.com/pranavkoushik0606-code/banking-atm-os-assignment
- Hosting: Render Free web service, Singapore region.
- Runtime: Docker container running the native C executable with POSIX pthreads.
- Backend source commit: `65fdf0b10d4a35ae41fcf89b10029c0037f9a22e`.
- Initial deployment: 23 November 2026; confirmed `live`.
- Automatic deployments are disabled. Later source changes need a manual deploy.

The HTML frontend and C API share this URL. Each browser gets separate fictional
accounts; balances are stored in process memory and reset on a server restart.
Free instances can sleep when idle, so a later visit may need time to start.

To manage or manually redeploy the service, open the Render dashboard for
`pranav-banking-atm` in My Workspace. The service runs on the free plan.
