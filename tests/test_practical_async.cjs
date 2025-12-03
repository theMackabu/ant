// Real-world async pattern: sequential operations
async function fetchUser(id) {
  console.log(`Fetching user ${id}...`);
  return new Promise(resolve => setTimeout(() => {
    resolve({ id, name: `User${id}` });
  }, 50));
}

async function fetchUserPosts(userId) {
  console.log(`Fetching posts for user ${userId}...`);
  return new Promise(resolve => setTimeout(() => {
    resolve([`Post1 by ${userId}`, `Post2 by ${userId}`]);
  }, 30));
}

async function getUserData(id) {
  const user = await fetchUser(id);
  console.log("Got user:", user.name);
  
  const posts = await fetchUserPosts(user.id);
  console.log("Got posts:", posts.length);
  
  return { user, posts };
}

// This is the common pattern - sequential async calls within one function
getUserData(123).then(data => {
  console.log("Complete!", data.user.name, "has", data.posts.length, "posts");
});
