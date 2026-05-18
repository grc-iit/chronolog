rs.initiate(
    {
          _id: "replconfig01",
          configsvr: true,
          members: [
                  { _id : 0, host : "ares-stor-25-40g:27017" },
                  { _id : 1, host : "ares-stor-26-40g:27017" }
                ]
        }
)
